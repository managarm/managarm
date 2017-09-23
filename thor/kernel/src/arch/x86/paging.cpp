
#include <frg/list.hpp>
#include "generic/kernel.hpp"

// --------------------------------------------------------
// Physical page access.
// --------------------------------------------------------

namespace thor {

void invlpg(const void *address) {
	auto p = reinterpret_cast<const char *>(address);
	asm volatile ("invlpg %0" : : "m"(*p));
}

void invalidatePage(const void *address) {
	auto p = reinterpret_cast<const char *>(address);
	asm volatile ("invlpg %0" : : "m"(*p));
}

struct PWindow {
private:
	struct Item {
		explicit Item(VirtualAddr address)
		: address{address}, numLocks{0} { }

		Item(const Item &) = delete;

		~Item() = delete; // TODO: This is not really intended.

		Item &operator= (const Item &) = delete;

		// Virtual address of this mapping.
		const VirtualAddr address;

		// Current physical access.
		PhysicalAddr physical;

		// Number of locks on this mapping.
		unsigned int numLocks;

		frg::default_list_hook<Item> queueHook;
	};

public:
	struct Handle {
		friend void swap(Handle &a, Handle &b) {
			using frigg::swap;
			swap(a._window, b._window);
			swap(a._item, b._item);
		}

		Handle()
		: _window{nullptr}, _item{nullptr} { }

		explicit Handle(PWindow &window, PhysicalAddr physical)
		: _window{&window}, _item{nullptr} {
			_item = _window->_acquire(physical);
		}

		Handle(const Handle &) = delete;

		Handle(Handle &&other)
		: Handle{} {
			swap(*this, other);
		}

		~Handle() {
			if(_item)
				_window->_release(_item);
		}

		Handle &operator= (Handle other) {
			swap(*this, other);
			return *this;
		}

		void *get() {
			assert(_item);
			return reinterpret_cast<void *>(_item->address);
		}

	private:
		PWindow *_window;
		Item *_item;
	};

	PWindow()
	: _numItems{0}, _numReclaimable{0}, _activeMap{frigg::DefaultHasher<VirtualAddr>{}, *kernelAlloc} { }

	void attach(VirtualAddr address) {
		auto item = frigg::construct<Item>(*kernelAlloc, address);
		_itemPool.push_front(item);
		_numItems++;
	}

private:
	Item *_acquire(PhysicalAddr physical) {
		auto it = _activeMap.get(physical);
		if(it) {
			auto item = *it;

			// Remove the item from the list of reclaimable items.
			if(item->numLocks++ == 0) {
				assert(item->queueHook.in_list);
				_reclaimQueue.erase(_reclaimQueue.iterator_to(item));
				_numReclaimable--;
			}

			return item;
		}else{
			assert(!_itemPool.empty());

			auto item = _itemPool.pop_front();
			item->physical = physical;
			item->numLocks = 1;
			_activeMap.insert(physical, item);
	
			KernelPageSpace::global().mapSingle4k(item->address, physical, page_access::write);
			
			return item;
		}
	}

	void _release(Item *item) {
		assert(item->numLocks);

		// Re-insert the item into the list of reclaimable items.
		if(--item->numLocks == 0) {
			_reclaimQueue.push_front(item);
			_numReclaimable++;
		}

		_reclaim();
	}

	void _reclaim() {
		assert(_numItems);
		while(2 * _numReclaimable >= _numItems) {
//			frigg::infoLogger() << "\e[31mReclaiming\e[39m" << frigg::endLog;
			auto item = _reclaimQueue.pop_back();
			_numReclaimable--;
			
			// Strategy: We have to performs the following actions:
			// - We deactive the item so that no once will lock it.
			// - We shoot down the mapping.
			// - We mark the item as re-usable.
			assert(!item->numLocks);
			_activeMap.remove(item->physical);

			KernelPageSpace::global().unmapSingle4k(item->address);
			invlpg(reinterpret_cast<void *>(item->address));

			_itemPool.push_front(item);
		}
	}

	size_t _numItems;

	// Number of items that can be reclaimed (i.e. they are not locked).
	size_t _numReclaimable;

	// This list stores all mappings that are currently not in use.
	frg::intrusive_list<
		Item,
		frg::locate_member<
			Item,
			frg::default_list_hook<Item>,
			&Item::queueHook
		>
	> _itemPool;

	frigg::Hashmap<
		PhysicalAddr,
		Item *,
		frigg::DefaultHasher<PhysicalAddr>,
		KernelAlloc
	> _activeMap;

	// Stores a LRU queue of all reclaimable mappings.
	frg::intrusive_list<
		Item,
		frg::locate_member<
			Item,
			frg::default_list_hook<Item>,
			&Item::queueHook
		>
	> _reclaimQueue;
};

frigg::LazyInitializer<PWindow> physicalWindow;

void initializePhysicalAccess() {
	physicalWindow.initialize();

	for(size_t progress = 0; progress < 0x200000; progress += kPageSize)
		physicalWindow->attach(0xFFFF'FF80'0060'0000 + progress);
}

struct PAccessor {
	PAccessor() = default;

	explicit PAccessor(PhysicalAddr physical)
	: _handle{*physicalWindow, physical} { }

	void *get() {
		return _handle.get();
	}

private:
	PWindow::Handle _handle;
};

} // namespace thor

// --------------------------------------------------------

enum {
	kPagePresent = 0x1,
	kPageWrite = 0x2,
	kPageUser = 0x4,
	kPageXd = 0x8000000000000000
};

namespace thor {

constexpr PhysicalWindow::PhysicalWindow(uint64_t *table, void *content)
: _table{table}, _content{content}, _locked{} { }

void *PhysicalWindow::acquire(PhysicalAddr physical) {
	assert(!(physical % kPageSize));

	// FIXME: This hack is uncredibly ugly!
	// Fix global initializers instead of using this hack!
	_table = reinterpret_cast<uint64_t *>(0xFFFF'FF80'0000'2000);
	_content = reinterpret_cast<uint64_t *>(0xFFFF'FF80'0040'0000);

	for(int i = 0; i < 512; ++i) {
		if(_locked[i])
			continue;

//		frigg::infoLogger() << "Locking " << i << " to " << (void *)physical << frigg::endLog;
		_locked[i] = true;
		_table[i] = physical | kPagePresent | kPageWrite | kPageXd;
		invlpg((char *)_content + i * kPageSize);
		return (char *)_content + i * kPageSize;
	}

	frigg::panicLogger() << "Cannot lock a slot of a PhysicalWindow" << frigg::endLog;
	__builtin_unreachable();
}

void PhysicalWindow::release(void *pointer) {
	assert(!(uintptr_t(pointer) % kPageSize));
	assert((char *)pointer >= (char *)_content);
	auto index = ((char *)pointer - (char *)_content) / kPageSize;

	_locked[index] = false;
	_table[index] = 0;
	invlpg((char *)_content + index * kPageSize);
}

PhysicalWindow generalWindow{reinterpret_cast<uint64_t *>(0xFFFF'FF80'0000'2000),
		reinterpret_cast<void *>(0xFFFF'FF80'0040'0000)};

// --------------------------------------------------------

PageBinding::PageBinding()
: _boundSpace{nullptr}, _alreadyShotSequence{0} { }

void PageBinding::rebind(PageSpace *space, PhysicalAddr pml4) {
	assert(!intsAreEnabled());

	// Shootdown everything.
	asm volatile ("mov %0, %%cr3" : : "r"(pml4) : "memory");
	
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
			for(size_t pg = 0; pg < current->size; pg += kPageSize)
				invlpg(reinterpret_cast<void *>(current->address + pg));

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
	uint64_t new_entry = physical | kPagePresent;
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

struct TableAccessor {
	TableAccessor() = default;

	TableAccessor(const TableAccessor &) = delete;

	TableAccessor &operator= (const TableAccessor &) = delete;
	
	void aim(PhysicalAddr address) {
		_accessor = PAccessor{address};
	}

	uint64_t &operator[] (size_t n) {
		return static_cast<uint64_t *>(_accessor.get())[n];
	}

private:
	PAccessor _accessor;
};

ClientPageSpace::ClientPageSpace() {
	_pml4Address = physicalAllocator->allocate(kPageSize);

	// Initialize the bottom half to unmapped memory.
	TableAccessor accessor;
	accessor.aim(_pml4Address);

	for(size_t i = 0; i < 256; i++)
		accessor[i] = 0;

	// Share the top half with the kernel.
	auto kernel_pml4 = KernelPageSpace::global().getPml4();
	auto kernel_table = (uint64_t *)SkeletalRegion::global().access(kernel_pml4);

	for(size_t i = 256; i < 512; i++) {
		assert(kernel_table[i] & kPagePresent);
		accessor[i] = kernel_table[i];
	}
}

ClientPageSpace::~ClientPageSpace() {
	assert(!"Implement this");
}

void ClientPageSpace::activate() {
	getCpuData()->primaryBinding.rebind(this, _pml4Address);
}

void ClientPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
		bool user_page, uint32_t flags) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);
	
	TableAccessor accessor4;
	TableAccessor accessor3;
	TableAccessor accessor2;
	TableAccessor accessor1;

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);

	// The PML4 does always exist.
	accessor4.aim(_pml4Address);

	// Make sure there is a PDPT.
	if(accessor4[index4] & kPagePresent) {
		accessor3.aim(accessor4[index4] & 0x000FFFFFFFFFF000);
	}else{
		auto tbl_address = physicalAllocator->allocate(kPageSize);
		accessor3.aim(tbl_address);
		for(int i = 0; i < 512; i++)
			accessor3[i] = 0;
		
		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		accessor4[index4] = new_entry;
	}
	assert(user_page ? ((accessor4[index4] & kPageUser) != 0)
			: ((accessor4[index4] & kPageUser) == 0));
	
	// Make sure there is a PD.
	if(accessor3[index3] & kPagePresent) {
		accessor2.aim(accessor3[index3] & 0x000FFFFFFFFFF000);
	}else{
		auto tbl_address = physicalAllocator->allocate(kPageSize);
		accessor2.aim(tbl_address);
		for(int i = 0; i < 512; i++)
			accessor2[i] = 0;
		
		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		accessor3[index3] = new_entry;
	}
	assert(user_page ? ((accessor3[index3] & kPageUser) != 0)
			: ((accessor3[index3] & kPageUser) == 0));
	
	// Make sure there is a PT.
	if(accessor2[index2] & kPagePresent) {
		accessor1.aim(accessor2[index2] & 0x000FFFFFFFFFF000);
	}else{
		auto tbl_address = physicalAllocator->allocate(kPageSize);
		accessor1.aim(tbl_address);
		for(int i = 0; i < 512; i++)
			accessor1[i] = 0;
		
		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		accessor2[index2] = new_entry;
	}
	assert(user_page ? ((accessor2[index2] & kPageUser) != 0)
			: ((accessor2[index2] & kPageUser) == 0));

	// Setup the new PTE.
	assert(!(accessor1[index1] & kPagePresent));
	uint64_t new_entry = physical | kPagePresent;
	if(user_page)
		new_entry |= kPageUser;
	if(flags & page_access::write)
		new_entry |= kPageWrite;
	if(!(flags & page_access::execute))
		new_entry |= kPageXd;
	accessor1[index1] = new_entry;
}

void ClientPageSpace::unmapRange(VirtualAddr pointer, size_t size, PageMode mode) {
	assert(!(pointer & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	TableAccessor accessor4;
	TableAccessor accessor3;
	TableAccessor accessor2;
	TableAccessor accessor1;

	for(size_t progress = 0; progress < size; progress += kPageSize) {
		auto index4 = (int)(((pointer + progress) >> 39) & 0x1FF);
		auto index3 = (int)(((pointer + progress) >> 30) & 0x1FF);
		auto index2 = (int)(((pointer + progress) >> 21) & 0x1FF);
		auto index1 = (int)(((pointer + progress) >> 12) & 0x1FF);
		
		// The PML4 is always present.
		accessor4.aim(_pml4Address);

		// Find the PDPT.
		if(mode == PageMode::remap && !(accessor4[index4] & kPagePresent))
			continue;
		assert(accessor4[index4] & kPagePresent);
		accessor3.aim(accessor4[index4] & 0x000FFFFFFFFFF000);
		
		// Find the PD.
		if(mode == PageMode::remap && !(accessor3[index3] & kPagePresent))
			continue;
		assert(accessor3[index3] & kPagePresent);
		accessor2.aim(accessor3[index3] & 0x000FFFFFFFFFF000);
		
		// Find the PT.
		if(mode == PageMode::remap && !(accessor2[index2] & kPagePresent))
			continue;
		assert(accessor2[index2] & kPagePresent);
		accessor1.aim(accessor2[index2] & 0x000FFFFFFFFFF000);
		
		if(mode == PageMode::remap && !(accessor1[index1] & kPagePresent))
			continue;
		assert(accessor1[index1] & kPagePresent);
		accessor1[index1] &= ~uint64_t{kPagePresent};
	}
}

bool ClientPageSpace::isMapped(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	TableAccessor accessor4;
	TableAccessor accessor3;
	TableAccessor accessor2;
	TableAccessor accessor1;

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);
	
	// The PML4 is always present.
	accessor4.aim(_pml4Address);

	// Find the PDPT.
	if(!(accessor4[index4] & kPagePresent))
		return false;
	accessor3.aim(accessor4[index4] & 0x000FFFFFFFFFF000);
	
	// Find the PD.
	if(!(accessor3[index3] & kPagePresent))
		return false;
	accessor2.aim(accessor3[index3] & 0x000FFFFFFFFFF000);
	
	// Find the PT.
	if(!(accessor2[index2] & kPagePresent))
		return false;
	accessor1.aim(accessor2[index2] & 0x000FFFFFFFFFF000);
	
	return accessor1[index1] & kPagePresent;
}

void thorRtInvalidateSpace() {
	asm volatile ("movq %%cr3, %%rax\n\t"
		"movq %%rax, %%cr3" : : : "%rax", "memory");
}

} // namespace thor

