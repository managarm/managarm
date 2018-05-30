
#include <arch/variable.hpp>
#include <frg/list.hpp>
#include "generic/kernel.hpp"

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

void initializePhysicalAccess() {
	// Nothing to do here.
}

} // namespace thor

// --------------------------------------------------------

enum {
	kPagePresent = 0x1,
	kPageWrite = 0x2,
	kPageUser = 0x4,
	kPageGlobal = 0x100,
	kPageXd = 0x8000000000000000
};

namespace thor {

// --------------------------------------------------------

PageContext::PageContext()
: _nextStamp{1}, _primaryBinding{nullptr} { }

PageBinding::PageBinding(int pcid)
: _pcid{pcid}, _boundSpace{nullptr}, _bindStamp{0}, _alreadyShotSequence{0} { }

void PageBinding::rebind(PageSpace *space, PhysicalAddr pml4) {
	assert(!intsAreEnabled());
	assert(getCpuData()->havePcids || !_pcid);

	auto context = &getCpuData()->pageContext;
	if(_boundSpace == space) {
		// If the space is bound AND we are the primary binding, we can omit changing CR3.
		if(context->_primaryBinding != this)
			asm volatile ("mov %0, %%cr3" : : "r"(pml4 | _pcid) : "memory");
	}else{
		// If we switch to another space, we have to invalidate PCIDs.
		if(getCpuData()->havePcids)
			invalidatePcid(_pcid);
		asm volatile ("mov %0, %%cr3" : : "r"(pml4 | _pcid) : "memory");
	}

	_bindStamp = context->_nextStamp++;
	context->_primaryBinding = this;

	// If we invalidated the PCID, mark everything as shot-down.
	if(_boundSpace == space)
		return;
	
	frg::intrusive_list<
		ShootNode,
		frg::locate_member<
			ShootNode,
			frg::default_list_hook<ShootNode>,
			&ShootNode::_queueNode
		>
	> complete;

	if(_boundSpace) {
		auto lock = frigg::guard(&_boundSpace->_mutex);

		if(!_boundSpace->_shootQueue.empty()) {
			auto current = _boundSpace->_shootQueue.back();
			while(current->_sequence > _alreadyShotSequence) {
				auto predecessor = current->_queueNode.previous;

				// Signal completion of the shootdown.
				if(current->_bindingsToShoot.fetch_sub(1, std::memory_order_acq_rel) == 1) {
					auto it = _boundSpace->_shootQueue.iterator_to(current);
					_boundSpace->_shootQueue.erase(it);
					complete.push_front(current);
				}

				if(!predecessor)
					break;
				current = predecessor;
			}
		}
		
		_boundSpace->_numBindings--;
	}

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->shotDown(current);
	}

	uint64_t target_seq;
	{
		auto lock = frigg::guard(&space->_mutex);

		target_seq = space->_shootSequence;
		space->_numBindings++;
	}

	_boundSpace = space;
	_alreadyShotSequence = target_seq;
}

void PageBinding::shootdown() {
	assert(!intsAreEnabled());
	
	if(!_boundSpace)
		return;
	
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
		auto lock = frigg::guard(&_boundSpace->_mutex);

		if(_boundSpace->_shootQueue.empty())
			return;
		
		target_seq = _boundSpace->_shootQueue.back()->_sequence;

		auto current = _boundSpace->_shootQueue.back();
		while(current->_sequence > _alreadyShotSequence) {
			auto predecessor = current->_queueNode.previous;

			// Perform the actual shootdown.
			assert(!(current->address & (kPageSize - 1)));
			assert(!(current->size & (kPageSize - 1)));

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

			if(!predecessor)
				break;
			current = predecessor;
		}
	}

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->shotDown(current);
	}

	_alreadyShotSequence = target_seq;
}

PageSpace::PageSpace()
: _numBindings{0}, _shootSequence{0} { }

void PageSpace::submitShootdown(ShootNode *node) {
	bool any_bindings;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		any_bindings = _numBindings;
		if(any_bindings) {
			node->_sequence = _shootSequence++;
			node->_bindingsToShoot = _numBindings;
			_shootQueue.push_back(node);
		}
	}

	if(any_bindings) {
		sendShootdownIpi();
	}else{
		node->shotDown(node);
	}
}

// --------------------------------------------------------
// Kernel paging management.
// --------------------------------------------------------

frigg::LazyInitializer<KernelPageSpace> kernelSpaceSingleton;

void KernelPageSpace::initialize(PhysicalAddr pml4_address) {
	kernelSpaceSingleton.initialize(pml4_address);
}

KernelPageSpace &KernelPageSpace::global() {
	return *kernelSpaceSingleton;
}

KernelPageSpace::KernelPageSpace(PhysicalAddr pml4_address)
: _pml4Address{pml4_address} { }

void KernelPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical, uint32_t flags) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);
	
	auto &region = SkeletalRegion::global();

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);

	// the pml4 exists already
	uint64_t *pml4_pointer = (uint64_t *)region.access(_pml4Address);

	// make sure there is a pdpt
	uint64_t pml4_initial_entry = pml4_pointer[pml4_index];
	uint64_t *pdpt_pointer;
	if((pml4_initial_entry & kPagePresent) != 0) {
		pdpt_pointer = (uint64_t *)region.access(pml4_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pdpt_page = SkeletalRegion::global().allocate();

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
		PhysicalAddr pd_page = SkeletalRegion::global().allocate();

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
		PhysicalAddr pt_page = SkeletalRegion::global().allocate();

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
	pt_pointer[pt_index] = new_entry;
}

PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert((pointer % 0x1000) == 0);

	auto &region = SkeletalRegion::global();

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);
	
	// find the pml4_entry
	uint64_t *pml4_pointer = (uint64_t *)region.access(_pml4Address);
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

PhysicalAddr KernelPageSpace::getPml4() {
	return _pml4Address;
}


// --------------------------------------------------------
// ClientPageSpace
// --------------------------------------------------------

ClientPageSpace::ClientPageSpace() {
	_pml4Address = physicalAllocator->allocate(kPageSize);

	// Initialize the bottom half to unmapped memory.
	PageAccessor accessor;
	accessor = PageAccessor{_pml4Address};
	auto tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor.get());

	for(size_t i = 0; i < 256; i++)
		tbl4[i].store(0);

	// Share the top half with the kernel.
	auto kernel_pml4 = KernelPageSpace::global().getPml4();
	auto kernel_table = (uint64_t *)SkeletalRegion::global().access(kernel_pml4);

	for(size_t i = 256; i < 512; i++) {
		assert(kernel_table[i] & kPagePresent);
		tbl4[i].store(kernel_table[i]);
	}
}

ClientPageSpace::~ClientPageSpace() {
	assert(!"Implement this");
}

void ClientPageSpace::activate() {
	auto bindings = getCpuData()->pcidBindings;

	// If PCIDs are not supported, we only use the first binding.
	if(!getCpuData()->havePcids) {
		bindings[0].rebind(this, _pml4Address);
		return;
	}

	int k = 0;
	for(int i = 0; i < maxPcidCount; i++) {
		// If the space is currently bound, always keep that binding.
		if(bindings[i].boundSpace() == this) {
			bindings[i].rebind(this, _pml4Address);
			return;
		}

		// Otherwise, prefer the LRU binding.
		if(bindings[i].bindStamp() < bindings[k].bindStamp())
			k = i;
	}

	bindings[k].rebind(this, _pml4Address);
}

void ClientPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
		bool user_page, uint32_t flags) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);
	
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
	accessor4 = PageAccessor{_pml4Address};

	// Make sure there is a PDPT.
	tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor4.get());
	if(tbl4[index4].load() & kPagePresent) {
		accessor3 = PageAccessor{tbl4[index4].load() & 0x000FFFFFFFFFF000};
	}else{
		auto tbl_address = physicalAllocator->allocate(kPageSize);
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
	tbl1[index1].store(new_entry);
}

void ClientPageSpace::unmapRange(VirtualAddr pointer, size_t size, PageMode mode) {
	assert(!(pointer & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	PageAccessor accessor4;
	PageAccessor accessor3;
	PageAccessor accessor2;
	PageAccessor accessor1;

	arch::scalar_variable<uint64_t> *tbl4;
	arch::scalar_variable<uint64_t> *tbl3;
	arch::scalar_variable<uint64_t> *tbl2;
	arch::scalar_variable<uint64_t> *tbl1;

	for(size_t progress = 0; progress < size; progress += kPageSize) {
		auto index4 = (int)(((pointer + progress) >> 39) & 0x1FF);
		auto index3 = (int)(((pointer + progress) >> 30) & 0x1FF);
		auto index2 = (int)(((pointer + progress) >> 21) & 0x1FF);
		auto index1 = (int)(((pointer + progress) >> 12) & 0x1FF);
		
		// The PML4 is always present.
		accessor4 = PageAccessor{_pml4Address};
		tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor4.get());

		// Find the PDPT.
		if(mode == PageMode::remap && !(tbl4[index4].load() & kPagePresent))
			continue;
		assert(tbl4[index4].load() & kPagePresent);
		accessor3 = PageAccessor{tbl4[index4].load() & 0x000FFFFFFFFFF000};
		tbl3 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor3.get());
		
		// Find the PD.
		if(mode == PageMode::remap && !(tbl3[index3].load() & kPagePresent))
			continue;
		assert(tbl3[index3].load() & kPagePresent);
		accessor2 = PageAccessor{tbl3[index3].load() & 0x000FFFFFFFFFF000};
		tbl2 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor2.get());
		
		// Find the PT.
		if(mode == PageMode::remap && !(tbl2[index2].load() & kPagePresent))
			continue;
		assert(tbl2[index2].load() & kPagePresent);
		accessor1 = PageAccessor{tbl2[index2].load() & 0x000FFFFFFFFFF000};
		tbl1 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor1.get());
		
		if(mode == PageMode::remap && !(tbl1[index1].load() & kPagePresent))
			continue;
		assert(tbl1[index1].load() & kPagePresent);
		tbl1[index1].store(tbl1[index1].load() & ~uint64_t{kPagePresent});
	}
}

bool ClientPageSpace::isMapped(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

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
	accessor4 = PageAccessor{_pml4Address};
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

} // namespace thor

