
#include <arch/variable.hpp>
#include <frg/list.hpp>
#include <thor-internal/core.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/arch/paging.hpp>

// --------------------------------------------------------
// Physical page access.
// --------------------------------------------------------

namespace thor {

void invalidatePage(const void *address) {
	auto p = reinterpret_cast<const char *>(address);
	asm volatile ("invlpg %0" : : "m"(*p) : "memory");
}

void invalidatePcid(int pcid) {
	struct {
		uint64_t pcid;
		const void *address;
	} descriptor;

	descriptor.pcid = pcid;
	descriptor.address = nullptr;

	uint64_t type = 1;
	asm volatile ("invpcid %1, %0" : : "r"(type), "m"(descriptor) : "memory");
}

void invalidatePage(int pcid, const void *address) {
	struct {
		uint64_t pcid;
		const void *address;
	} descriptor;

	descriptor.pcid = pcid;
	descriptor.address = address;

	uint64_t type = 0;
	asm volatile ("invpcid %1, %0" : : "r"(type), "m"(descriptor) : "memory");
}

void invalidateFullTlb() {
	uint64_t pml4;
	asm volatile ("mov %%cr3, %0" : "=r"(pml4));
	pml4 &= 0x000FFFFFFFFFF000; // Clear the first bit to invalidate the PCID.
	asm volatile ("mov %0, %%cr3" : : "r"(pml4) : "memory");
}

void poisonPhysicalAccess(PhysicalAddr physical) {
	auto address = 0xFFFF'8000'0000'0000 + physical;
	KernelPageSpace::global().unmapSingle4k(address);
	invalidatePage(reinterpret_cast<void *>(address));
}

void poisonPhysicalWriteAccess(PhysicalAddr physical) {
	auto address = 0xFFFF'8000'0000'0000 + physical;
	KernelPageSpace::global().unmapSingle4k(address);
	KernelPageSpace::global().mapSingle4k(address, physical, 0, CachingMode::null);
	invalidatePage(reinterpret_cast<void *>(address));
}

} // namespace thor

// --------------------------------------------------------

enum {
	kPagePresent = 0x1,
	kPageWrite = 0x2,
	kPageUser = 0x4,
	kPagePwt = 0x8,
	kPagePcd = 0x10,
	kPageDirty = 0x40,
	kPagePat = 0x80,
	kPageGlobal = 0x100,
	kPageXd = 0x8000000000000000,
	kPageAddress = 0x000FFFFFFFFFF000
};

namespace thor {

// --------------------------------------------------------

PageContext::PageContext()
: _nextStamp{1}, _primaryBinding{nullptr} { }

PageBinding::PageBinding()
: _pcid{0}, _boundSpace{nullptr},
		_primaryStamp{0}, _alreadyShotSequence{0} { }

bool PageBinding::isPrimary() {
	assert(!intsAreEnabled());
	assert(getCpuData()->havePcids || !_pcid);
	auto context = &getCpuData()->pageContext;

	return context->_primaryBinding == this;
}

void PageBinding::rebind() {
	assert(!intsAreEnabled());
	assert(getCpuData()->havePcids || !_pcid);
	assert(_boundSpace);
	auto context = &getCpuData()->pageContext;

	auto cr3 = _boundSpace->rootTable() | _pcid;
	if(getCpuData()->havePcids)
		cr3 |= PhysicalAddr(1) << 63; // Do not invalidate the PCID.
	asm volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");

	_primaryStamp = context->_nextStamp++;
	context->_primaryBinding = this;
}

void PageBinding::rebind(smarter::shared_ptr<PageSpace> space) {
	assert(!intsAreEnabled());
	assert(getCpuData()->havePcids || !_pcid);
	assert(!_boundSpace || _boundSpace.get() != space.get()); // This would be unnecessary work.
	auto context = &getCpuData()->pageContext;

	auto unbound_space = _boundSpace;
	auto unbound_sequence = _alreadyShotSequence;

	// Bind the new space.
	uint64_t target_seq;
	{
		auto lock = frg::guard(&space->_mutex);

		target_seq = space->_shootSequence;
		space->_numBindings++;
	}

	_boundSpace = space;
	_alreadyShotSequence = target_seq;

	// Switch CR3 and invalidate the PCID.
	auto cr3 = space->rootTable() | _pcid;
	asm volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");

	_primaryStamp = context->_nextStamp++;
	context->_primaryBinding = this;

	// Mark every shootdown request in the unbound space as shot-down.
	frg::intrusive_list<
		ShootNode,
		frg::locate_member<
			ShootNode,
			frg::default_list_hook<ShootNode>,
			&ShootNode::_queueNode
		>
	> complete;

	if(unbound_space) {
		auto lock = frg::guard(&unbound_space->_mutex);

		if(!unbound_space->_shootQueue.empty()) {
			auto current = unbound_space->_shootQueue.back();
			while(current->_sequence > unbound_sequence) {
				auto predecessor = current->_queueNode.previous;

				// Signal completion of the shootdown.
				if(current->_initiatorCpu != getCpuData()) {
					if(current->_bindingsToShoot.fetch_sub(1, std::memory_order_acq_rel) == 1) {
						auto it = unbound_space->_shootQueue.iterator_to(current);
						unbound_space->_shootQueue.erase(it);
						complete.push_front(current);
					}
				}

				if(!predecessor)
					break;
				current = predecessor;
			}
		}

		unbound_space->_numBindings--;
		if(!unbound_space->_numBindings && unbound_space->_retireNode) {
			unbound_space->_retireNode->complete();
			unbound_space->_retireNode = nullptr;
		}
	}

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->complete();
	}
}

void PageBinding::unbind() {
	assert(!intsAreEnabled());

	if(!_boundSpace)
		return;

	// Perform shootdown.
	if(isPrimary()) {
		// Switch to the kernel CR3 and invalidate the PCID.
		auto cr3 = KernelPageSpace::global().rootTable() | _pcid;
		asm volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
	}else{
		// If there was only a single binding, it would have been primary.
		assert(getCpuData()->havePcids);
		invalidatePcid(_pcid);
	}

	frg::intrusive_list<
		ShootNode,
		frg::locate_member<
			ShootNode,
			frg::default_list_hook<ShootNode>,
			&ShootNode::_queueNode
		>
	> complete;

	{
		auto lock = frg::guard(&_boundSpace->_mutex);

		if(!_boundSpace->_shootQueue.empty()) {
			auto current = _boundSpace->_shootQueue.back();
			while(current->_sequence > _alreadyShotSequence) {
				auto predecessor = current->_queueNode.previous;

				// The actual shootdown was done above.
				// Signal completion of the shootdown.
				if(current->_initiatorCpu != getCpuData()) {
					if(current->_bindingsToShoot.fetch_sub(1, std::memory_order_acq_rel) == 1) {
						auto it = _boundSpace->_shootQueue.iterator_to(current);
						_boundSpace->_shootQueue.erase(it);
						complete.push_front(current);
					}
				}

				if(!predecessor)
					break;
				current = predecessor;
			}
		}

		_boundSpace->_numBindings--;
		if(!_boundSpace->_numBindings && _boundSpace->_retireNode) {
			_boundSpace->_retireNode->complete();
			_boundSpace->_retireNode = nullptr;
		}
	}

	_boundSpace = nullptr;
	_alreadyShotSequence = 0;

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->complete();
	}
}

void PageBinding::shootdown() {
	assert(!intsAreEnabled());

	if(!_boundSpace)
		return;

	// If we retire the space anyway, just flush the whole PCID.
	if(_boundSpace->_wantToRetire.load(std::memory_order_acquire)) {
		unbind();
		return;
	}

	frg::intrusive_list<
		ShootNode,
		frg::locate_member<
			ShootNode,
			frg::default_list_hook<ShootNode>,
			&ShootNode::_queueNode
		>
	> complete;

	uint64_t target_seq;
	{
		auto lock = frg::guard(&_boundSpace->_mutex);

		if(!_boundSpace->_shootQueue.empty()) {
			auto current = _boundSpace->_shootQueue.back();
			while(current->_sequence > _alreadyShotSequence) {
				auto predecessor = current->_queueNode.previous;

				if(current->_initiatorCpu != getCpuData()) {
					// Perform the actual shootdown.
					if(!getCpuData()->havePcids) {
						assert(!_pcid);
						for(size_t pg = 0; pg < current->size; pg += kPageSize)
							invalidatePage(reinterpret_cast<void *>(current->address + pg));
					}else{
						for(size_t pg = 0; pg < current->size; pg += kPageSize)
							invalidatePage(_pcid, reinterpret_cast<void *>(current->address + pg));
					}

					// Signal completion of the shootdown.
					if(current->_bindingsToShoot.fetch_sub(1, std::memory_order_acq_rel) == 1) {
						auto it = _boundSpace->_shootQueue.iterator_to(current);
						_boundSpace->_shootQueue.erase(it);
						complete.push_front(current);
					}
				}

				if(!predecessor)
					break;
				current = predecessor;
			}
		}
		target_seq = _boundSpace->_shootSequence;
	}

	_alreadyShotSequence = target_seq;

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->complete();
	}
}

// --------------------------------------------------------

GlobalPageBinding::GlobalPageBinding()
: _alreadyShotSequence{0} { }

void GlobalPageBinding::bind() {
	assert(!intsAreEnabled());

	auto space = &KernelPageSpace::global();

	uint64_t targetSeq;
	{
		auto lock = frg::guard(&space->_shootMutex);

		targetSeq = space->_shootSequence;
		space->_numBindings++;
	}

	_alreadyShotSequence = targetSeq;
}

void GlobalPageBinding::shootdown() {
	assert(!intsAreEnabled());

	auto space = &KernelPageSpace::global();

	frg::intrusive_list<
		ShootNode,
		frg::locate_member<
			ShootNode,
			frg::default_list_hook<ShootNode>,
			&ShootNode::_queueNode
		>
	> complete;

	uint64_t targetSeq;
	{
		auto lock = frg::guard(&space->_shootMutex);

		if(!space->_shootQueue.empty()) {
			auto current = space->_shootQueue.back();
			while(current->_sequence > _alreadyShotSequence) {
				auto predecessor = current->_queueNode.previous;

				if(current->_initiatorCpu != getCpuData()) {
					// Perform the actual shootdown.
					for(size_t pg = 0; pg < current->size; pg += kPageSize)
						invalidatePage(reinterpret_cast<void *>(current->address + pg));

					// Signal completion of the shootdown.
					if(current->_bindingsToShoot.fetch_sub(1, std::memory_order_acq_rel) == 1) {
						auto it = space->_shootQueue.iterator_to(current);
						space->_shootQueue.erase(it);
						complete.push_front(current);
					}
				}

				if(!predecessor)
					break;
				current = predecessor;
			}
		}
		targetSeq = space->_shootSequence;
	}

	_alreadyShotSequence = targetSeq;

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->complete();
	}
}

// --------------------------------------------------------
// PageSpace.
// --------------------------------------------------------

void PageSpace::activate(smarter::shared_ptr<PageSpace> space) {
	auto bindings = getCpuData()->pcidBindings;

	int k = 0;
	for(int i = 0; i < maxPcidCount; i++) {
		// If the space is currently bound, always keep that binding.
		auto bound = bindings[i].boundSpace();
		if(bound && bound.get() == space.get()) {
			if(!bindings[i].isPrimary())
				bindings[i].rebind();
			return;
		}

		// If PCIDs are not supported, we only use the first binding.
		if(!getCpuData()->havePcids)
			break;

		// Otherwise, prefer the LRU binding.
		if(bindings[i].primaryStamp() < bindings[k].primaryStamp())
			k = i;
	}

	bindings[k].rebind(space);
}


PageSpace::PageSpace(PhysicalAddr root_table)
: _rootTable{root_table}, _numBindings{0}, _shootSequence{0} { }

PageSpace::~PageSpace() {
	assert(!_numBindings);
}

void PageSpace::retire(RetireNode *node) {
	bool any_bindings;
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		any_bindings = _numBindings;
		if(any_bindings) {
			_retireNode = node;
			_wantToRetire.store(true, std::memory_order_release);
		}
	}

	if(!any_bindings)
		node->complete();

	sendShootdownIpi();
}

bool PageSpace::submitShootdown(ShootNode *node) {
	assert(!(node->address & (kPageSize - 1)));
	assert(!(node->size & (kPageSize - 1)));

	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		auto unshot_bindings = _numBindings;

		// Perform synchronous shootdown.
		auto bindings = getCpuData()->pcidBindings;
		if(!getCpuData()->havePcids) {
			if(bindings[0].boundSpace().get() == this) {
				assert(unshot_bindings);

				for(size_t pg = 0; pg < node->size; pg += kPageSize)
					invalidatePage(reinterpret_cast<void *>(node->address + pg));
				unshot_bindings--;
			}
		}else{
			for(int i = 0; i < maxPcidCount; i++) {
				if(bindings[i].boundSpace().get() != this)
					continue;
				assert(unshot_bindings);

				for(size_t pg = 0; pg < node->size; pg += kPageSize)
					invalidatePage(bindings[i].getPcid(),
							reinterpret_cast<void *>(node->address + pg));
				unshot_bindings--;
			}
		}

		if(!unshot_bindings)
			return true;

		node->_initiatorCpu = getCpuData();
		node->_sequence = ++_shootSequence;
		node->_bindingsToShoot = unshot_bindings;
		_shootQueue.push_back(node);
	}

	sendShootdownIpi();
	return false;
}

// --------------------------------------------------------
// Kernel paging management.
// --------------------------------------------------------

frg::manual_box<KernelPageSpace> kernelSpaceSingleton;

void KernelPageSpace::initialize() {
	PhysicalAddr pml4_ptr;
	asm volatile ("mov %%cr3, %0" : "=r" (pml4_ptr));

	kernelSpaceSingleton.initialize(pml4_ptr);
}

KernelPageSpace &KernelPageSpace::global() {
	return *kernelSpaceSingleton;
}

KernelPageSpace::KernelPageSpace(PhysicalAddr pml4_address)
: _rootTable{pml4_address} { }

bool KernelPageSpace::submitShootdown(ShootNode *node) {
	assert(!(node->address & (kPageSize - 1)));
	assert(!(node->size & (kPageSize - 1)));

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		auto unshotBindings = _numBindings;

		// Perform synchronous shootdown.
		assert(unshotBindings);
		for(size_t pg = 0; pg < node->size; pg += kPageSize)
			invalidatePage(reinterpret_cast<void *>(node->address + pg));
		unshotBindings--;

		if(!unshotBindings)
			return true;

		node->_initiatorCpu = getCpuData();
		node->_sequence = ++_shootSequence;
		node->_bindingsToShoot = unshotBindings;
		_shootQueue.push_back(node);
	}

	sendShootdownIpi();
	return false;
}

void KernelPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
		uint32_t flags, CachingMode caching_mode) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto &region = SkeletalRegion::global();

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);

	// the pml4 exists already
	uint64_t *pml4_pointer = (uint64_t *)region.access(rootTable());

	// make sure there is a pdpt
	uint64_t pml4_initial_entry = pml4_pointer[pml4_index];
	uint64_t *pdpt_pointer;
	if((pml4_initial_entry & kPagePresent) != 0) {
		pdpt_pointer = (uint64_t *)region.access(pml4_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pdpt_page = physicalAllocator->allocate(kPageSize);
		assert(pdpt_page != static_cast<PhysicalAddr>(-1) && "OOM");

		pdpt_pointer = (uint64_t *)region.access(pdpt_page);
		for(int i = 0; i < 512; i++)
			pdpt_pointer[i] = 0;

		uint64_t new_entry = pdpt_page | kPagePresent | kPageWrite;
		pml4_pointer[pml4_index] = new_entry;
	}
	assert(!(pml4_pointer[pml4_index] & kPageUser));

	// make sure there is a pd
	uint64_t pdpt_initial_entry = pdpt_pointer[pdpt_index];
	uint64_t *pd_pointer;
	if((pdpt_initial_entry & kPagePresent) != 0) {
		pd_pointer = (uint64_t *)region.access(pdpt_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pd_page = physicalAllocator->allocate(kPageSize);
		assert(pd_page != static_cast<PhysicalAddr>(-1) && "OOM");

		pd_pointer = (uint64_t *)region.access(pd_page);
		for(int i = 0; i < 512; i++)
			pd_pointer[i] = 0;

		uint64_t new_entry = pd_page | kPagePresent | kPageWrite;
		pdpt_pointer[pdpt_index] = new_entry;
	}
	assert(!(pdpt_pointer[pdpt_index] & kPageUser));

	// make sure there is a pt
	uint64_t pd_initial_entry = pd_pointer[pd_index];
	uint64_t *pt_pointer;
	if((pd_initial_entry & kPagePresent) != 0) {
		pt_pointer = (uint64_t *)region.access(pd_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pt_page = physicalAllocator->allocate(kPageSize);
		assert(pt_page != static_cast<PhysicalAddr>(-1) && "OOM");

		pt_pointer = (uint64_t *)region.access(pt_page);
		for(int i = 0; i < 512; i++)
			pt_pointer[i] = 0;

		uint64_t new_entry = pt_page | kPagePresent | kPageWrite;
		pd_pointer[pd_index] = new_entry;
	}
	assert(!(pd_pointer[pd_index] & kPageUser));

	// setup the new pt entry
	assert((pt_pointer[pt_index] & kPagePresent) == 0);
	uint64_t new_entry = physical | kPagePresent | kPageGlobal;
	if(flags & page_access::write)
		new_entry |= kPageWrite;
	if(!(flags & page_access::execute))
		new_entry |= kPageXd;
	if(caching_mode == CachingMode::writeThrough) {
		new_entry |= kPagePwt;
	}else if(caching_mode == CachingMode::writeCombine) {
		new_entry |= kPagePat | kPagePwt;
	}else if(caching_mode == CachingMode::uncached) {
		new_entry |= kPagePwt | kPagePcd | kPagePat;
	}else{
		assert(caching_mode == CachingMode::null || caching_mode == CachingMode::writeBack);
	}
	pt_pointer[pt_index] = new_entry;
}

PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert((pointer % 0x1000) == 0);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto &region = SkeletalRegion::global();

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);

	// find the pml4_entry
	uint64_t *pml4_pointer = (uint64_t *)region.access(rootTable());
	uint64_t pml4_entry = pml4_pointer[pml4_index];

	// find the pdpt entry
	assert((pml4_entry & kPagePresent) != 0);
	uint64_t *pdpt_pointer = (uint64_t *)region.access(pml4_entry & 0x000FFFFFFFFFF000);
	uint64_t pdpt_entry = pdpt_pointer[pdpt_index];

	// find the pd entry
	assert((pdpt_entry & kPagePresent) != 0);
	uint64_t *pd_pointer = (uint64_t *)region.access(pdpt_entry & 0x000FFFFFFFFFF000);
	uint64_t pd_entry = pd_pointer[pd_index];

	// find the pt entry
	assert((pd_entry & kPagePresent) != 0);
	uint64_t *pt_pointer = (uint64_t *)region.access(pd_entry & 0x000FFFFFFFFFF000);

	// change the pt entry
	assert((pt_pointer[pt_index] & kPagePresent) != 0);
	pt_pointer[pt_index] ^= kPagePresent;
	return pt_pointer[pt_index] & 0x000FFFFFFFFFF000;
}

// --------------------------------------------------------
// ClientPageSpace
// --------------------------------------------------------

ClientPageSpace::ClientPageSpace()
: PageSpace{physicalAllocator->allocate(kPageSize)} {
	assert(rootTable() != PhysicalAddr(-1) && "OOM");

	// Initialize the bottom half to unmapped memory.
	PageAccessor accessor;
	accessor = PageAccessor{rootTable()};
	auto tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor.get());

	for(size_t i = 0; i < 256; i++)
		tbl4[i].store(0);

	// Share the top half with the kernel.
	auto kernel_pml4 = KernelPageSpace::global().rootTable();
	auto kernel_table = (uint64_t *)SkeletalRegion::global().access(kernel_pml4);

	for(size_t i = 256; i < 512; i++) {
		assert(kernel_table[i] & kPagePresent);
		tbl4[i].store(kernel_table[i]);
	}
}

ClientPageSpace::~ClientPageSpace() {
	auto clearLevel2 = [&] (PhysicalAddr ps) {
		PageAccessor accessor{ps};
		auto tbl = reinterpret_cast<uint64_t *>(accessor.get());
		for(int i = 0; i < 512; i++) {
			if(tbl[i] & kPagePresent)
				physicalAllocator->free(tbl[i] & kPageAddress, kPageSize);
		}
	};

	auto clearLevel3 = [&] (PhysicalAddr ps) {
		PageAccessor accessor{ps};
		auto tbl = reinterpret_cast<uint64_t *>(accessor.get());
		for(int i = 0; i < 512; i++) {
			if(!(tbl[i] & kPagePresent))
				continue;
			clearLevel2(tbl[i] & kPageAddress);
			physicalAllocator->free(tbl[i] & kPageAddress, kPageSize);
		}
	};

	// From PML4, we only clear the lower half (higher half is shared with kernel).
	PageAccessor root_accessor{rootTable()};
	auto root_tbl = reinterpret_cast<uint64_t *>(root_accessor.get());
	for(int i = 0; i < 256; i++) {
		if(!(root_tbl[i] & kPagePresent))
			continue;
		clearLevel3(root_tbl[i] & kPageAddress);
		physicalAllocator->free(root_tbl[i] & kPageAddress, kPageSize);
	}

	physicalAllocator->free(rootTable(), kPageSize);
}

void ClientPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
		bool user_page, uint32_t flags, CachingMode caching_mode) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	PageAccessor accessor4;
	PageAccessor accessor3;
	PageAccessor accessor2;
	PageAccessor accessor1;

	arch::scalar_variable<uint64_t> *tbl4;
	arch::scalar_variable<uint64_t> *tbl3;
	arch::scalar_variable<uint64_t> *tbl2;
	arch::scalar_variable<uint64_t> *tbl1;

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);

	// The PML4 does always exist.
	accessor4 = PageAccessor{rootTable()};

	// Make sure there is a PDPT.
	tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor4.get());
	if(tbl4[index4].load() & kPagePresent) {
		accessor3 = PageAccessor{tbl4[index4].load() & 0x000FFFFFFFFFF000};
	}else{
		auto tbl_address = physicalAllocator->allocate(kPageSize);
		assert(tbl_address != PhysicalAddr(-1) && "OOM");
		accessor3 = PageAccessor{tbl_address};
		memset(accessor3.get(), 0, kPageSize);

		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		tbl4[index4].store(new_entry);
	}
	assert(user_page ? ((tbl4[index4].load() & kPageUser) != 0)
			: ((tbl4[index4].load() & kPageUser) == 0));

	// Make sure there is a PD.
	tbl3 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor3.get());
	if(tbl3[index3].load() & kPagePresent) {
		accessor2 = PageAccessor{tbl3[index3].load() & 0x000FFFFFFFFFF000};
	}else{
		auto tbl_address = physicalAllocator->allocate(kPageSize);
		assert(tbl_address != PhysicalAddr(-1) && "OOM");
		accessor2 = PageAccessor{tbl_address};
		memset(accessor2.get(), 0, kPageSize);

		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		tbl3[index3].store(new_entry);
	}
	assert(user_page ? ((tbl3[index3].load() & kPageUser) != 0)
			: ((tbl3[index3].load() & kPageUser) == 0));

	// Make sure there is a PT.
	tbl2 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor2.get());
	if(tbl2[index2].load() & kPagePresent) {
		accessor1 = PageAccessor{tbl2[index2].load() & 0x000FFFFFFFFFF000};
	}else{
		auto tbl_address = physicalAllocator->allocate(kPageSize);
		assert(tbl_address != PhysicalAddr(-1) && "OOM");
		accessor1 = PageAccessor{tbl_address};
		memset(accessor1.get(), 0, kPageSize);

		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		tbl2[index2].store(new_entry);
	}
	assert(user_page ? ((tbl2[index2].load() & kPageUser) != 0)
			: ((tbl2[index2].load() & kPageUser) == 0));

	// Setup the new PTE.
	tbl1 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor1.get());
	assert(!(tbl1[index1].load() & kPagePresent));
	uint64_t new_entry = physical | kPagePresent;
	if(user_page)
		new_entry |= kPageUser;
	if(flags & page_access::write)
		new_entry |= kPageWrite;
	if(!(flags & page_access::execute))
		new_entry |= kPageXd;
	if(caching_mode == CachingMode::writeThrough) {
		new_entry |= kPagePwt;
	}else if(caching_mode == CachingMode::writeCombine) {
		new_entry |= kPagePat | kPagePwt;
	}else{
		assert(caching_mode == CachingMode::null || caching_mode == CachingMode::writeBack);
	}
	tbl1[index1].store(new_entry);
}

PageStatus ClientPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	PageAccessor accessor4;
	PageAccessor accessor3;
	PageAccessor accessor2;
	PageAccessor accessor1;

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);

	// The PML4 is always present.
	accessor4 = PageAccessor{rootTable()};
	auto tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor4.get());

	// Find the PDPT.
	if(!(tbl4[index4].load() & kPagePresent))
		return 0;
	assert(tbl4[index4].load() & kPagePresent);
	accessor3 = PageAccessor{tbl4[index4].load() & 0x000FFFFFFFFFF000};
	auto tbl3 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor3.get());

	// Find the PD.
	if(!(tbl3[index3].load() & kPagePresent))
		return 0;
	assert(tbl3[index3].load() & kPagePresent);
	accessor2 = PageAccessor{tbl3[index3].load() & 0x000FFFFFFFFFF000};
	auto tbl2 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor2.get());

	// Find the PT.
	if(!(tbl2[index2].load() & kPagePresent))
		return 0;
	assert(tbl2[index2].load() & kPagePresent);
	accessor1 = PageAccessor{tbl2[index2].load() & 0x000FFFFFFFFFF000};
	auto tbl1 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor1.get());

	// TODO: Do we want to preserve some bits?
	auto bits = tbl1[index1].atomic_exchange(0);
	if(!(bits & kPagePresent))
		return 0;

	PageStatus status = page_status::present;
	if(bits & kPageDirty)
		status |= page_status::dirty;
	return status;
}

PageStatus ClientPageSpace::cleanSingle4k(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	PageAccessor accessor4;
	PageAccessor accessor3;
	PageAccessor accessor2;
	PageAccessor accessor1;

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);

	// The PML4 is always present.
	accessor4 = PageAccessor{rootTable()};
	auto tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor4.get());

	// Find the PDPT.
	if(!(tbl4[index4].load() & kPagePresent))
		return 0;
	assert(tbl4[index4].load() & kPagePresent);
	accessor3 = PageAccessor{tbl4[index4].load() & 0x000FFFFFFFFFF000};
	auto tbl3 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor3.get());

	// Find the PD.
	if(!(tbl3[index3].load() & kPagePresent))
		return 0;
	assert(tbl3[index3].load() & kPagePresent);
	accessor2 = PageAccessor{tbl3[index3].load() & 0x000FFFFFFFFFF000};
	auto tbl2 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor2.get());

	// Find the PT.
	if(!(tbl2[index2].load() & kPagePresent))
		return 0;
	assert(tbl2[index2].load() & kPagePresent);
	accessor1 = PageAccessor{tbl2[index2].load() & 0x000FFFFFFFFFF000};
	auto tbl1 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor1.get());

	auto bits = tbl1[index1].load();
	if(!(bits & kPagePresent))
		return 0;
	PageStatus status = page_status::present;
	if(bits & kPageDirty) {
		status |= page_status::dirty;
		tbl1[index1].atomic_exchange(bits & ~kPageDirty);
	}
	return status;
}

bool ClientPageSpace::isMapped(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	PageAccessor accessor4;
	PageAccessor accessor3;
	PageAccessor accessor2;
	PageAccessor accessor1;

	arch::scalar_variable<uint64_t> *tbl4;
	arch::scalar_variable<uint64_t> *tbl3;
	arch::scalar_variable<uint64_t> *tbl2;
	arch::scalar_variable<uint64_t> *tbl1;

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);

	// The PML4 is always present.
	accessor4 = PageAccessor{rootTable()};
	tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor4.get());

	// Find the PDPT.
	if(!(tbl4[index4].load() & kPagePresent))
		return false;
	accessor3 = PageAccessor{tbl4[index4].load() & 0x000FFFFFFFFFF000};
	tbl3 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor3.get());

	// Find the PD.
	if(!(tbl3[index3].load() & kPagePresent))
		return false;
	accessor2 = PageAccessor{tbl3[index3].load() & 0x000FFFFFFFFFF000};
	tbl2 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor2.get());

	// Find the PT.
	if(!(tbl2[index2].load() & kPagePresent))
		return false;
	accessor1 = PageAccessor{tbl2[index2].load() & 0x000FFFFFFFFFF000};
	tbl1 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor1.get());

	return tbl1[index1].load() & kPagePresent;
}

bool ClientPageSpace::updatePageAccess(VirtualAddr) {
	return false;
}

ClientPageSpace::Walk::Walk(ClientPageSpace *space)
: _space{space} {
	irqMutex().lock();
	_space->_mutex.lock();
}

ClientPageSpace::Walk::~Walk() {
	_space->_mutex.unlock();
	irqMutex().unlock();
}

void ClientPageSpace::Walk::walkTo(uintptr_t address) {
	assert(!(_address & (kPageSize - 1)));

	_address = address;
	_accessor4 = PageAccessor{};
	_accessor3 = PageAccessor{};
	_accessor2 = PageAccessor{};
	_accessor1 = PageAccessor{};
}

PageFlags ClientPageSpace::Walk::peekFlags() {
	_update();
	assert(_accessor1);

	auto tbl = reinterpret_cast<arch::scalar_variable<uint64_t> *>(_accessor1.get());
	auto ent = tbl[(_address >> 12) & 0x1FF].load();
	assert(ent & kPagePresent);

	PageFlags flags = 0;
	if(ent & kPageWrite)
		flags |= page_access::write;
	if(!(ent & kPageXd))
		flags |= page_access::execute;
	return flags;
}

PhysicalAddr ClientPageSpace::Walk::peekPhysical() {
	_update();
	assert(_accessor1);

	auto tbl = reinterpret_cast<arch::scalar_variable<uint64_t> *>(_accessor1.get());
	auto ent = tbl[(_address >> 12) & 0x1FF].load();
	assert(ent & kPagePresent);
	return ent & 0x000FFFFFFFFFF000;
}

void ClientPageSpace::Walk::_update() {
	auto index4 = (int)((_address >> 39) & 0x1FF);
	auto index3 = (int)((_address >> 30) & 0x1FF);
	auto index2 = (int)((_address >> 21) & 0x1FF);

	// The PML4 does always exist.
	_accessor4 = PageAccessor{_space->rootTable()};

	// Make sure there is a PDPT.
	auto tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(_accessor4.get());
	if(!(tbl4[index4].load() & kPagePresent))
		return;
	_accessor3 = PageAccessor{tbl4[index4].load() & 0x000FFFFFFFFFF000};

	// Make sure there is a PD.
	auto tbl3 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(_accessor3.get());
	if(!(tbl3[index3].load() & kPagePresent))
		return;
	_accessor2 = PageAccessor{tbl3[index3].load() & 0x000FFFFFFFFFF000};

	// Make sure there is a PT.
	auto tbl2 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(_accessor2.get());
	if(!(tbl2[index2].load() & kPagePresent))
		return;
	_accessor1 = PageAccessor{tbl2[index2].load() & 0x000FFFFFFFFFF000};
}

} // namespace thor

