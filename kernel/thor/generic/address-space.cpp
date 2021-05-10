#include <cstddef>
#include <type_traits>
#include <thor-internal/address-space.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/fiber.hpp>
#include <frg/container_of.hpp>
#include <thor-internal/types.hpp>

namespace thor {

extern size_t kernelMemoryUsage;

namespace {
	constexpr bool logCleanup = false;
	constexpr bool logUsage = false;

	void logRss(VirtualSpace *space) {
		if(!logUsage)
			return;
		auto rss = space->rss();
		if(!rss)
			return;
		auto b = 63 -__builtin_clz(rss);
		if(b < 1)
			return;
		if(rss & ((1 << (b - 1)) - 1))
			return;
		infoLogger() << "thor: RSS of " << space << " increases above "
				<< (rss / 1024) << " KiB" << frg::endlog;
		infoLogger() << "thor:     Physical usage: "
				<< (physicalAllocator->numUsedPages() * 4) << " KiB, kernel usage: "
				<< (kernelMemoryUsage / 1024) << " KiB" << frg::endlog;
	}
}

MemorySlice::MemorySlice(smarter::shared_ptr<MemoryView> view,
		ptrdiff_t view_offset, size_t view_size)
: _view{std::move(view)}, _viewOffset{view_offset}, _viewSize{view_size} {
	assert(!(_viewOffset & (kPageSize - 1)));
	assert(!(_viewSize & (kPageSize - 1)));
}

// --------------------------------------------------------
// HoleAggregator
// --------------------------------------------------------

bool HoleAggregator::aggregate(Hole *hole) {
	size_t size = hole->length();
	if(HoleTree::get_left(hole) && HoleTree::get_left(hole)->largestHole > size)
		size = HoleTree::get_left(hole)->largestHole;
	if(HoleTree::get_right(hole) && HoleTree::get_right(hole)->largestHole > size)
		size = HoleTree::get_right(hole)->largestHole;

	if(hole->largestHole == size)
		return false;
	hole->largestHole = size;
	return true;
}

bool HoleAggregator::check_invariant(HoleTree &tree, Hole *hole) {
	auto pred = tree.predecessor(hole);
	auto succ = tree.successor(hole);

	// Check largest hole invariant.
	size_t size = hole->length();
	if(tree.get_left(hole) && tree.get_left(hole)->largestHole > size)
		size = tree.get_left(hole)->largestHole;
	if(tree.get_right(hole) && tree.get_right(hole)->largestHole > size)
		size = tree.get_right(hole)->largestHole;

	if(hole->largestHole != size) {
		infoLogger() << "largestHole violation: " << "Expected " << size
				<< ", got " << hole->largestHole << "." << frg::endlog;
		return false;
	}

	// Check non-overlapping memory areas invariant.
	if(pred && hole->address() < pred->address() + pred->length()) {
		infoLogger() << "Non-overlapping (left) violation" << frg::endlog;
		return false;
	}
	if(succ && hole->address() + hole->length() > succ->address()) {
		infoLogger() << "Non-overlapping (right) violation" << frg::endlog;
		return false;
	}

	return true;
}

// --------------------------------------------------------
// Mapping
// --------------------------------------------------------

Mapping::Mapping(size_t length, MappingFlags flags,
		smarter::shared_ptr<MemorySlice> slice_, uintptr_t viewOffset)
: length{length}, flags{flags},
		slice{std::move(slice_)}, viewOffset{viewOffset} {
	assert(viewOffset >= slice->offset());
	assert(viewOffset + length <= slice->offset() + slice->length());
	view = slice->getView();
}

Mapping::~Mapping() {
	assert(state == MappingState::retired);
	//infoLogger() << "\e[31mthor: Mapping is destructed\e[39m" << frg::endlog;
}

void Mapping::tie(smarter::shared_ptr<VirtualSpace> newOwner, VirtualAddr address) {
	assert(!owner);
	assert(newOwner);
	owner = std::move(newOwner);
	this->address = address;
}

void Mapping::protect(MappingFlags protectFlags) {
	std::underlying_type_t<MappingFlags> newFlags = flags;
	newFlags &= ~(MappingFlags::protRead | MappingFlags::protWrite | MappingFlags::protExecute);
	newFlags |= protectFlags;
	flags = static_cast<MappingFlags>(newFlags);
}

void Mapping::populateVirtualRange(uintptr_t offset, size_t size,
		smarter::shared_ptr<WorkQueue> wq, PopulateVirtualRangeNode *node) {
	async::detach_with_allocator(*kernelAlloc, [] (Mapping *self,
			uintptr_t offset, size_t size,
			smarter::shared_ptr<WorkQueue> wq, PopulateVirtualRangeNode *node) -> coroutine<void> {
		size_t progress = 0;
		while(progress < size) {
			// Avoid stack overflows.
			co_await wq->schedule();

			auto outcome = co_await self->touchVirtualPage(offset + progress, wq);
			if(!outcome) {
				node->result = outcome.error();
				node->resume();
				co_return;
			}
			progress += outcome.value().range.get<1>();
		}
		node->result = frg::success;
		node->resume();
	}(this, offset, size, std::move(wq), node));
}

uint32_t Mapping::compilePageFlags() {
	uint32_t pageFlags = 0;
	// TODO: Allow inaccessible mappings.
	assert(flags & MappingFlags::protRead);
	if(flags & MappingFlags::protWrite)
		pageFlags |= page_access::write;
	if(flags & MappingFlags::protExecute)
		pageFlags |= page_access::execute;
	return pageFlags;
}

void Mapping::lockVirtualRange(uintptr_t offset, size_t size,
		smarter::shared_ptr<WorkQueue> wq, LockVirtualRangeNode *node) {
	// This can be removed if we change the return type of asyncLockRange to frg::expected.
	auto transformError = [node] (Error e) {
		if(e == Error::success) {
			node->result = {};
		}else{
			node->result = e;
		}
		node->resume();
	};
	async::detach_with_allocator(*kernelAlloc,
			async::transform(view->asyncLockRange(viewOffset + offset, size, std::move(wq)),
					transformError));
}

void Mapping::unlockVirtualRange(uintptr_t offset, size_t size) {
	view->unlockRange(viewOffset + offset, size);
}

frg::tuple<PhysicalAddr, CachingMode>
Mapping::resolveRange(ptrdiff_t offset) {
	assert(state == MappingState::active);

	// TODO: This function should be rewritten.
	assert((size_t)offset + kPageSize <= length);
	auto bundle_range = view->peekRange(viewOffset + offset);
	return frg::tuple<PhysicalAddr, CachingMode>{bundle_range.get<0>(), bundle_range.get<1>()};
}

void Mapping::touchVirtualPage(uintptr_t offset,
		smarter::shared_ptr<WorkQueue> wq, TouchVirtualPageNode *node) {
	assert(state == MappingState::active);

	async::detach_with_allocator(*kernelAlloc, [] (Mapping *self, uintptr_t offset,
			smarter::shared_ptr<WorkQueue> wq, TouchVirtualPageNode *node) -> coroutine<void> {
		FetchFlags fetchFlags = 0;
		if(self->flags & MappingFlags::dontRequireBacking)
			fetchFlags |= FetchNode::disallowBacking;

		if(auto e = co_await self->view->asyncLockRange(
				(self->viewOffset + offset) & ~(kPageSize - 1), kPageSize,
				wq); e != Error::success)
			assert(!"asyncLockRange() failed");

		auto [error, range, rangeFlags] = co_await self->view->fetchRange(
				self->viewOffset + offset, wq);

		// TODO: Update RSS, handle dirty pages, etc.
		auto pageOffset = self->address + offset;

		// Do not call into markDirty() with a lock held.
		PageStatus status;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&self->pagingMutex);

			status = self->owner->_ops->unmapSingle4k(pageOffset & ~(kPageSize - 1));
			self->owner->_ops->mapSingle4k(pageOffset & ~(kPageSize - 1),
					range.get<0>() & ~(kPageSize - 1),
					self->compilePageFlags(), range.get<2>());
		}

		if(status & page_status::present) {
			if(status & page_status::dirty)
				self->view->markDirty(self->viewOffset + offset, kPageSize);
		}else{
			self->owner->_residuentSize += kPageSize;
			logRss(self->owner.get());
		}

		self->view->unlockRange((self->viewOffset + offset) & ~(kPageSize - 1), kPageSize);

		node->result = TouchVirtualResult{range, false};
		node->resume();
	}(this, offset, std::move(wq), node));
}

coroutine<void> Mapping::runEvictionLoop() {
	while(true) {
		auto eviction = co_await view->pollEviction(&observer, cancelEviction);
		if(!eviction)
			break;
		if(eviction.offset() + eviction.size() <= viewOffset
				|| eviction.offset() >= viewOffset + length) {
			eviction.done();
			continue;
		}

		co_await evictionMutex.async_lock();

		// Begin and end offsets of the region that we need to unmap.
		auto shootBegin = frg::max(eviction.offset(), viewOffset);
		auto shootEnd = frg::min(eviction.offset() + eviction.size(),
				viewOffset + length);

		// Offset from the beginning of the mapping.
		auto shootOffset = shootBegin - viewOffset;
		auto shootSize = shootEnd - shootBegin;
		assert(shootSize);
		assert(!(shootOffset & (kPageSize - 1)));
		assert(!(shootSize & (kPageSize - 1)));

		// Unmap the memory range.
		for(size_t pg = 0; pg < shootSize; pg += kPageSize) {
			// Do not call into markDirty() with a lock held.
			PageStatus status;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&pagingMutex);

				status = owner->_ops->unmapSingle4k(address + shootOffset + pg);
			}

			if(!(status & page_status::present))
				continue;
			if(status & page_status::dirty)
				view->markDirty(viewOffset + shootOffset + pg, kPageSize);
			owner->_residuentSize -= kPageSize;
			logRss(owner.get());
		}

		co_await owner->_ops->shootdown(address + shootOffset, shootSize);

		eviction.done();

		evictionMutex.unlock();
	}

	evictionDoneEvent.raise();
}

// --------------------------------------------------------
// CowMapping
// --------------------------------------------------------

CowChain::CowChain(smarter::shared_ptr<CowChain> chain)
: _superChain{std::move(chain)}, _pages{*kernelAlloc} {
}

CowChain::~CowChain() {
	if(logCleanup)
		infoLogger() << "thor: Releasing CowChain" << frg::endlog;

	for(auto it = _pages.begin(); it != _pages.end(); ++it) {
		auto physical = it->load(std::memory_order_relaxed);
		assert(physical != PhysicalAddr(-1));
		physicalAllocator->free(physical, kPageSize);
	}
}

// --------------------------------------------------------
// VirtualSpace
// --------------------------------------------------------

VirtualSpace::VirtualSpace(VirtualOperations *ops)
: _ops{ops} { }

void VirtualSpace::setupInitialHole(VirtualAddr address, size_t size) {
	auto hole = frg::construct<Hole>(*kernelAlloc, address, size);
	_holes.insert(hole);
}

VirtualSpace::~VirtualSpace() {
	if(logCleanup)
		infoLogger() << "\e[31mthor: VirtualSpace is destructed\e[39m" << frg::endlog;

	while(_holes.get_root()) {
		auto hole = _holes.get_root();
		_holes.remove(hole);
		frg::destruct(*kernelAlloc, hole);
	}
}

void VirtualSpace::retire() {
	if(logCleanup)
		infoLogger() << "\e[31mthor: VirtualSpace is cleared\e[39m" << frg::endlog;

	// TODO: Set some flag to make sure that no mappings are added/deleted.
	auto mapping = _mappings.first();
	while(mapping) {
		assert(mapping->state == MappingState::active);
		mapping->state = MappingState::zombie;

		for(size_t progress = 0; progress < mapping->length; progress += kPageSize) {
			VirtualAddr vaddr = mapping->address + progress;

			// Do not call into markDirty() with a lock held.
			PageStatus status;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&mapping->pagingMutex);

				status = _ops->unmapSingle4k(vaddr);
			}

			if(!(status & page_status::present))
				continue;
			if(status & page_status::dirty)
				mapping->view->markDirty(mapping->viewOffset + progress, kPageSize);
			_residuentSize -= kPageSize;
			logRss(this);
		}

		mapping = MappingTree::successor(mapping);
	}

	// TODO: It would be less ugly to run this in a non-detached way.
	async::detach_with_allocator(*kernelAlloc, [] (smarter::shared_ptr<VirtualSpace> self)
			-> coroutine<void> {
		co_await self->_ops->retire();

		while(self->_mappings.get_root()) {
			auto mapping = self->_mappings.get_root();
			self->_mappings.remove(mapping);

			assert(mapping->state == MappingState::zombie);
			mapping->state = MappingState::retired;

			if(mapping->view->canEvictMemory()) {
				mapping->cancelEviction.cancel();
				co_await mapping->evictionDoneEvent.wait();
			}
			mapping->view->removeObserver(&mapping->observer);
			mapping->selfPtr.ctr()->decrement();
		}
	}(selfPtr.lock()));
}

smarter::shared_ptr<Mapping> VirtualSpace::getMapping(VirtualAddr address) {
	auto irq_lock = frg::guard(&irqMutex());
	auto space_guard = frg::guard(&_mutex);

	return _findMapping(address);
}

bool VirtualSpace::map(smarter::borrowed_ptr<MemorySlice> slice,
		VirtualAddr address, size_t offset, size_t length, uint32_t flags,
		MapNode *node) {
	assert(length);
	assert(!(length % kPageSize));

	if(offset + length > slice->length()) {
		node->nodeResult_.emplace(Error::bufferTooSmall);
		return true;
	}

	// The shared_ptr to the new Mapping needs to survive until the locks are released.
	VirtualAddr actualAddress;
	smarter::shared_ptr<Mapping> mapping;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto spaceLock = frg::guard(&_mutex);

		if(flags & kMapFixed) {
			assert(address);
			assert((address % kPageSize) == 0);
			actualAddress = _allocateAt(address, length);
		}else{
			actualAddress = _allocate(length, flags);
		}
		assert(actualAddress);

	//	infoLogger() << "Creating new mapping at " << (void *)actualAddress
	//			<< ", length: " << (void *)length << frg::endlog;

		// Setup a new Mapping object.
		std::underlying_type_t<MappingFlags> mappingFlags = 0;

		// TODO: The upgrading mechanism needs to be arch-specific:
		// Some archs might only support RX, while other support X.
		auto mask = kMapProtRead | kMapProtWrite | kMapProtExecute;
		if((flags & mask) == (kMapProtRead | kMapProtWrite | kMapProtExecute)
				|| (flags & mask) == (kMapProtWrite | kMapProtExecute)) {
			// WX is upgraded to RWX.
			mappingFlags |= MappingFlags::protRead | MappingFlags::protWrite
				| MappingFlags::protExecute;
		}else if((flags & mask) == (kMapProtRead | kMapProtExecute)
				|| (flags & mask) == kMapProtExecute) {
			// X is upgraded to RX.
			mappingFlags |= MappingFlags::protRead | MappingFlags::protExecute;
		}else if((flags & mask) == (kMapProtRead | kMapProtWrite)
				|| (flags & mask) == kMapProtWrite) {
			// W is upgraded to RW.
			mappingFlags |= MappingFlags::protRead | MappingFlags::protWrite;
		}else if((flags & mask) == kMapProtRead) {
			mappingFlags |= MappingFlags::protRead;
		}else{
			assert(!(flags & mask));
		}

		if(flags & kMapDontRequireBacking)
			mappingFlags |= MappingFlags::dontRequireBacking;

		mapping = smarter::allocate_shared<Mapping>(Allocator{},
				length, static_cast<MappingFlags>(mappingFlags),
				slice.lock(), slice->offset() + offset);
		mapping->selfPtr = mapping;

		assert(!(flags & kMapPopulate));

		// Install the new mapping object.
		mapping->tie(selfPtr.lock(), actualAddress);
		_mappings.insert(mapping.get());

		assert(mapping->state == MappingState::null);
		mapping->state = MappingState::active;

		// We keep one reference until the detach the observer.
		mapping.ctr()->increment();
		mapping->view->addObserver(&mapping->observer);

		uint32_t pageFlags = 0;
		if((mappingFlags & MappingFlags::permissionMask) & MappingFlags::protWrite)
			pageFlags |= page_access::write;
		if((mappingFlags & MappingFlags::permissionMask) & MappingFlags::protExecute)
			pageFlags |= page_access::execute;
		// TODO: Allow inaccessible mappings.
		assert((mappingFlags & MappingFlags::permissionMask) & MappingFlags::protRead);

		for(size_t progress = 0; progress < mapping->length; progress += kPageSize) {
			auto physicalRange = mapping->view->peekRange(mapping->viewOffset + progress);

			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mapping->pagingMutex);

			VirtualAddr vaddr = mapping->address + progress;
			assert(!_ops->isMapped(vaddr));
			if(physicalRange.get<0>() != PhysicalAddr(-1)) {
				_ops->mapSingle4k(vaddr, physicalRange.get<0>(),
						pageFlags, physicalRange.get<1>());
				_residuentSize += kPageSize;
				logRss(this);
			}
		}

	}

	// Only enable eviction after the peekRange() loop above.
	// Since eviction is not yet enabled in that loop, we do not have
	// to take the evictionMutex.
	if(mapping->view->canEvictMemory())
		async::detach_with_allocator(*kernelAlloc, mapping->runEvictionLoop());

	node->nodeResult_.emplace(actualAddress);
	return true;
}

bool VirtualSpace::protect(VirtualAddr address, size_t length,
		uint32_t flags, AddressProtectNode *node) {
	async::detach_with_allocator(*kernelAlloc, [] (VirtualSpace *self,
			VirtualAddr address, size_t length,
			uint32_t flags, AddressProtectNode *node) -> coroutine<void> {
		std::underlying_type_t<MappingFlags> mappingFlags = 0;

		// TODO: The upgrading mechanism needs to be arch-specific:
		// Some archs might only support RX, while other support X.
		auto mask = kMapProtRead | kMapProtWrite | kMapProtExecute;
		if((flags & mask) == (kMapProtRead | kMapProtWrite | kMapProtExecute)
				|| (flags & mask) == (kMapProtWrite | kMapProtExecute)) {
			// WX is upgraded to RWX.
			mappingFlags |= MappingFlags::protRead | MappingFlags::protWrite
				| MappingFlags::protExecute;
		}else if((flags & mask) == (kMapProtRead | kMapProtExecute)
				|| (flags & mask) == kMapProtExecute) {
			// X is upgraded to RX.
			mappingFlags |= MappingFlags::protRead | MappingFlags::protExecute;
		}else if((flags & mask) == (kMapProtRead | kMapProtWrite)
				|| (flags & mask) == kMapProtWrite) {
			// W is upgraded to RW.
			mappingFlags |= MappingFlags::protRead | MappingFlags::protWrite;
		}else if((flags & mask) == kMapProtRead) {
			mappingFlags |= MappingFlags::protRead;
		}else{
			assert(!(flags & mask));
		}

		smarter::shared_ptr<Mapping> mapping;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto spaceGuard = frg::guard(&self->_mutex);

			mapping = self->_findMapping(address);
		}
		assert(mapping);

		// TODO: Allow shrinking of the mapping.
		assert(mapping->address == address);
		assert(mapping->length == length);
		mapping->protect(static_cast<MappingFlags>(mappingFlags));

		assert(mapping->state == MappingState::active);

		uint32_t pageFlags = 0;
		if((mapping->flags & MappingFlags::permissionMask) & MappingFlags::protWrite)
			pageFlags |= page_access::write;
		if((mapping->flags & MappingFlags::permissionMask) & MappingFlags::protExecute)
			pageFlags |= page_access::execute;
		// TODO: Allow inaccessible mappings.
		assert((mapping->flags & MappingFlags::permissionMask) & MappingFlags::protRead);

		co_await mapping->evictionMutex.async_lock();

		for(size_t progress = 0; progress < mapping->length; progress += kPageSize) {
			auto physicalRange = mapping->view->peekRange(mapping->viewOffset + progress);

			VirtualAddr vaddr = mapping->address + progress;

			// Do not call into markDirty() with a lock held.
			PageStatus status;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&mapping->pagingMutex);

				status = self->_ops->unmapSingle4k(vaddr);
				if(physicalRange.get<0>() != PhysicalAddr(-1))
					self->_ops->mapSingle4k(vaddr, physicalRange.get<0>(),
							pageFlags, physicalRange.get<1>());
			}

			if(status & page_status::present) {
				if(status & page_status::dirty)
					mapping->view->markDirty(mapping->viewOffset + progress, kPageSize);
				if(physicalRange.get<0>() == PhysicalAddr(-1))
					self->_residuentSize -= kPageSize;
			}else{
				if(physicalRange.get<0>() != PhysicalAddr(-1))
					self->_residuentSize += kPageSize;
			}
			logRss(self);
		}

		mapping->evictionMutex.unlock();

		co_await self->_ops->shootdown(address, length);
		node->complete();
	}(this, address, length, flags, node));

	return false;
}

bool VirtualSpace::unmap(VirtualAddr address, size_t length, AddressUnmapNode *node) {
	smarter::shared_ptr<Mapping> mapping;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		mapping = _findMapping(address);
		assert(mapping);

		// TODO: Allow shrinking of the mapping.
		assert(mapping->address == address);
		assert(mapping->length == length);

		assert(mapping->state == MappingState::active);
		mapping->state = MappingState::zombie;
	}

	// Mark pages as dirty and unmap without holding a lock.
	for(size_t progress = 0; progress < mapping->length; progress += kPageSize) {
		VirtualAddr vaddr = mapping->address + progress;

		// Do not call into markDirty() with a lock held.
		PageStatus status;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&mapping->pagingMutex);

			status = _ops->unmapSingle4k(vaddr);
		}

		if(!(status & page_status::present))
			continue;
		if(status & page_status::dirty)
			mapping->view->markDirty(mapping->viewOffset + progress, kPageSize);
		_residuentSize -= kPageSize;
		logRss(this);
	}

	static constexpr auto deleteMapping = [] (VirtualSpace *space, Mapping *mapping) {
		space->_mappings.remove(mapping);

		assert(mapping->state == MappingState::zombie);
		mapping->state = MappingState::retired;

		if(mapping->view->canEvictMemory())
			mapping->cancelEviction.cancel();

		// TODO: It would be less ugly to run this in a non-detached way.
		auto cleanUpObserver = [] (Mapping *mapping) -> coroutine<void> {
			if(mapping->view->canEvictMemory())
				co_await mapping->evictionDoneEvent.wait();
			mapping->view->removeObserver(&mapping->observer);
			mapping->selfPtr.ctr()->decrement();
		};
		async::detach_with_allocator(*kernelAlloc, cleanUpObserver(mapping));
	};

	static constexpr auto closeHole = [] (VirtualSpace *space, VirtualAddr address, size_t length) {
		// Find the holes that preceede/succeede mapping.
		Hole *pre;
		Hole *succ;

		auto current = space->_holes.get_root();
		while(true) {
			assert(current);
			if(address < current->address()) {
				if(HoleTree::get_left(current)) {
					current = HoleTree::get_left(current);
				}else{
					pre = HoleTree::predecessor(current);
					succ = current;
					break;
				}
			}else{
				assert(address >= current->address() + current->length());
				if(HoleTree::get_right(current)) {
					current = HoleTree::get_right(current);
				}else{
					pre = current;
					succ = HoleTree::successor(current);
					break;
				}
			}
		}

		// Try to merge the new hole and the existing ones.
		if(pre && pre->address() + pre->length() == address
				&& succ && address + length == succ->address()) {
			auto hole = frg::construct<Hole>(*kernelAlloc, pre->address(),
					pre->length() + length + succ->length());

			space->_holes.remove(pre);
			space->_holes.remove(succ);
			space->_holes.insert(hole);
			frg::destruct(*kernelAlloc, pre);
			frg::destruct(*kernelAlloc, succ);
		}else if(pre && pre->address() + pre->length() == address) {
			auto hole = frg::construct<Hole>(*kernelAlloc,
					pre->address(), pre->length() + length);

			space->_holes.remove(pre);
			space->_holes.insert(hole);
			frg::destruct(*kernelAlloc, pre);
		}else if(succ && address + length == succ->address()) {
			auto hole = frg::construct<Hole>(*kernelAlloc,
					address, length + succ->length());

			space->_holes.remove(succ);
			space->_holes.insert(hole);
			frg::destruct(*kernelAlloc, succ);
		}else{
			auto hole = frg::construct<Hole>(*kernelAlloc,
					address, length);

			space->_holes.insert(hole);
		}
	};

	async::detach_with_allocator(*kernelAlloc,
			async::transform(_ops->shootdown(address, length), [=] () {
		deleteMapping(this, mapping.get());
		closeHole(this, address, length);
		node->complete();
	}));

	return false;
}

void VirtualSpace::synchronize(VirtualAddr address, size_t size, SynchronizeNode *node) {
	auto misalign = address & (kPageSize - 1);
	auto alignedAddress = address & ~(kPageSize - 1);
	auto alignedSize = (size + misalign + kPageSize - 1) & ~(kPageSize - 1);

	async::detach_with_allocator(*kernelAlloc, [] (VirtualSpace *self,
			VirtualAddr alignedAddress, size_t alignedSize,
			SynchronizeNode *node) -> coroutine<void> {
		size_t overallProgress = 0;
		while(overallProgress < alignedSize) {
			smarter::shared_ptr<Mapping> mapping;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto spaceGuard = frg::guard(&self->_mutex);

				mapping = self->_findMapping(alignedAddress + overallProgress);
			}
			assert(mapping);

			auto mappingOffset = alignedAddress + overallProgress - mapping->address;
			auto mappingChunk = frg::min(alignedSize - overallProgress,
					mapping->length - mappingOffset);
			assert(mapping->state == MappingState::active);
			assert(mappingOffset + mappingChunk <= mapping->length);

			for(size_t chunkProgress = 0; chunkProgress < mappingChunk;
					chunkProgress += kPageSize) {
				VirtualAddr vaddr = mapping->address + mappingOffset + chunkProgress;

				// Do not call into markDirty() with a lock held.
				PageStatus status;
				{
					auto irqLock = frg::guard(&irqMutex());
					auto lock = frg::guard(&mapping->pagingMutex);

					status = self->_ops->cleanSingle4k(vaddr);
				}

				if(!(status & page_status::present))
					continue;
				if(status & page_status::dirty)
					mapping->view->markDirty(mapping->viewOffset + chunkProgress, kPageSize);
			}
			overallProgress += mappingChunk;
		}
		co_await self->_ops->shootdown(alignedAddress, alignedSize);

		node->resume();
	}(this, alignedAddress, alignedSize, node));
}

frg::optional<bool>
VirtualSpace::handleFault(VirtualAddr address, uint32_t faultFlags,
		smarter::shared_ptr<WorkQueue> wq, FaultNode *node) {
	smarter::shared_ptr<Mapping> mapping;
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto space_guard = frg::guard(&_mutex);

		mapping = _findMapping(address);
	}
	if(!mapping)
		return false;

	// Here we do the mapping-based fault handling.
	if(faultFlags & VirtualSpace::kFaultWrite)
		if(!((mapping->flags & MappingFlags::permissionMask) & MappingFlags::protWrite)) {
			return true;
		}
	if(faultFlags & VirtualSpace::kFaultExecute)
		if(!((mapping->flags & MappingFlags::permissionMask) & MappingFlags::protExecute)) {
			return true;
		}

	async::detach_with_allocator(*kernelAlloc, [] (smarter::shared_ptr<Mapping> mapping,
			uintptr_t address,
			smarter::shared_ptr<WorkQueue> wq, FaultNode *node) -> coroutine<void> {
		auto faultPage = (address - mapping->address) & ~(kPageSize - 1);
		auto outcome = co_await mapping->touchVirtualPage(faultPage, std::move(wq));
		if(!outcome) {
			node->complete(false);
			co_return;
		}

		// Spurious page faults are the result of race conditions.
		// They should be rare. If they happen too often, something is probably wrong!
		if(outcome.value().spurious)
				infoLogger() << "\e[33m" "thor: Spurious page fault"
						"\e[39m" << frg::endlog;
		node->complete(true);
	}(std::move(mapping), address, std::move(wq), node));

	return {};
}

smarter::shared_ptr<Mapping> VirtualSpace::_findMapping(VirtualAddr address) {
	auto current = _mappings.get_root();
	while(current) {
		if(address < current->address) {
			current = MappingTree::get_left(current);
		}else if(address >= current->address + current->length) {
			current = MappingTree::get_right(current);
		}else{
			assert(address >= current->address
					&& address < current->address + current->length);
			return current->selfPtr.lock();
		}
	}

	return nullptr;
}

VirtualAddr VirtualSpace::_allocate(size_t length, MapFlags flags) {
	assert(length > 0);
	assert((length % kPageSize) == 0);
//	infoLogger() << "Allocate virtual memory area"
//			<< ", size: 0x" << frg::hex_fmt(length) << frg::endlog;

	if(_holes.get_root()->largestHole < length)
		return 0; // TODO: Return something else here?

	auto current = _holes.get_root();
	while(true) {
		if(flags & kMapPreferBottom) {
			// Try to allocate memory at the bottom of the range.
			if(HoleTree::get_left(current)
					&& HoleTree::get_left(current)->largestHole >= length) {
				current = HoleTree::get_left(current);
				continue;
			}

			if(current->length() >= length) {
				// Note that _splitHole can deallocate the hole!
				auto address = current->address();
				_splitHole(current, 0, length);
				return address;
			}

			assert(HoleTree::get_right(current));
			assert(HoleTree::get_right(current)->largestHole >= length);
			current = HoleTree::get_right(current);
		}else{
			// Try to allocate memory at the top of the range.
			assert(flags & kMapPreferTop);

			if(HoleTree::get_right(current)
					&& HoleTree::get_right(current)->largestHole >= length) {
				current = HoleTree::get_right(current);
				continue;
			}

			if(current->length() >= length) {
				// Note that _splitHole can deallocate the hole!
				auto offset = current->length() - length;
				auto address = current->address() + offset;
				_splitHole(current, offset, length);
				return address;
			}

			assert(HoleTree::get_left(current));
			assert(HoleTree::get_left(current)->largestHole >= length);
			current = HoleTree::get_left(current);
		}
	}
}

VirtualAddr VirtualSpace::_allocateAt(VirtualAddr address, size_t length) {
	assert(!(address % kPageSize));
	assert(!(length % kPageSize));

	auto current = _holes.get_root();
	while(true) {
		// TODO: Otherwise, this method fails.
		assert(current);

		if(address < current->address()) {
			current = HoleTree::get_left(current);
		}else if(address >= current->address() + current->length()) {
			current = HoleTree::get_right(current);
		}else{
			assert(address >= current->address()
					&& address < current->address() + current->length());
			break;
		}
	}

	_splitHole(current, address - current->address(), length);
	return address;
}

void VirtualSpace::_splitHole(Hole *hole, VirtualAddr offset, size_t length) {
	assert(length);
	assert(offset + length <= hole->length());

	_holes.remove(hole);

	if(offset) {
		auto predecessor = frg::construct<Hole>(*kernelAlloc, hole->address(), offset);
		_holes.insert(predecessor);
	}

	if(offset + length < hole->length()) {
		auto successor = frg::construct<Hole>(*kernelAlloc,
				hole->address() + offset + length, hole->length() - (offset + length));
		_holes.insert(successor);
	}

	frg::destruct(*kernelAlloc, hole);
}

coroutine<size_t> readPartialVirtualSpace(VirtualSpace *space, uintptr_t address,
		void *buffer, size_t size, smarter::shared_ptr<WorkQueue> wq) {
	size_t progress = 0;
	while(progress < size) {
		auto mapping = space->getMapping(address + progress);
		if(!mapping)
			co_return progress;

		auto startInMapping = address + progress - mapping->address;
		auto limitInMapping = frg::min(size - progress, mapping->length - startInMapping);
		// Otherwise, getMapping() would have returned garbage.
		assert(limitInMapping);

		auto lockOutcome = co_await mapping->lockVirtualRange(startInMapping, limitInMapping, wq);
		if(!lockOutcome)
			co_return progress;

		// This loop iterates until we hit the end of the mapping.
		bool success = true;
		while(progress < size) {
			auto offsetInMapping = address + progress - mapping->address;
			if(offsetInMapping == mapping->length)
				break;
			assert(offsetInMapping < mapping->length);

			// Ensure that the page is available.
			// TODO: there is no real reason why we need to page aligned here; however, the
			//       touchVirtualPage() code does not handle the unaligned code correctly so far.
			auto touchOutcome = co_await mapping->touchVirtualPage(
					offsetInMapping & ~(kPageSize - 1), wq);
			if(!touchOutcome) {
				success = false;
				break;
			}

			auto [physical, cacheMode] = mapping->resolveRange(
					offsetInMapping & ~(kPageSize - 1));
			// Since we have locked the MemoryView, the physical address remains valid here.
			assert(physical != PhysicalAddr(-1));

			// Do heavy copying on the WQ.
			co_await wq->schedule();

			PageAccessor accessor{physical};
			auto misalign = offsetInMapping & (kPageSize - 1);
			auto chunk = frg::min(size - progress, kPageSize - misalign);
			assert(chunk); // Otherwise, we would have finished already.
			memcpy(reinterpret_cast<std::byte *>(buffer) + progress,
					reinterpret_cast<const std::byte *>(accessor.get()) + misalign,
					chunk);
			progress += chunk;
		}

		mapping->unlockVirtualRange(startInMapping, limitInMapping);

		if(!success)
			co_return progress;
	}

	co_return progress;
}

coroutine<size_t> writePartialVirtualSpace(VirtualSpace *space, uintptr_t address,
		const void *buffer, size_t size, smarter::shared_ptr<WorkQueue> wq) {
	size_t progress = 0;
	while(progress < size) {
		auto mapping = space->getMapping(address + progress);
		if(!mapping)
			co_return progress;

		auto startInMapping = address + progress - mapping->address;
		auto limitInMapping = frg::min(size - progress, mapping->length - startInMapping);
		// Otherwise, getMapping() would have returned garbage.
		assert(limitInMapping);

		auto lockOutcome = co_await mapping->lockVirtualRange(startInMapping, limitInMapping, wq);
		if(!lockOutcome)
			co_return progress;

		// This loop iterates until we hit the end of the mapping.
		bool success = true;
		while(progress < size) {
			auto offsetInMapping = address + progress - mapping->address;
			if(offsetInMapping == mapping->length)
				break;
			assert(offsetInMapping < mapping->length);

			// Ensure that the page is available.
			// TODO: there is no real reason why we need to page aligned here; however, the
			//       touchVirtualPage() code does not handle the unaligned code correctly so far.
			auto touchOutcome = co_await mapping->touchVirtualPage(
					offsetInMapping & ~(kPageSize - 1), wq);
			if(!touchOutcome) {
				success = false;
				break;
			}

			auto [physical, cacheMode] = mapping->resolveRange(
					offsetInMapping & ~(kPageSize - 1));
			// Since we have locked the MemoryView, the physical address remains valid here.
			assert(physical != PhysicalAddr(-1));

			// Do heavy copying on the WQ.
			co_await wq->schedule();

			PageAccessor accessor{physical};
			auto misalign = offsetInMapping & (kPageSize - 1);
			auto chunk = frg::min(size - progress, kPageSize - misalign);
			assert(chunk); // Otherwise, we would have finished already.
			memcpy(reinterpret_cast<std::byte *>(accessor.get()) + misalign,
					reinterpret_cast<const std::byte *>(buffer) + progress,
					chunk);
			progress += chunk;
		}

		mapping->unlockVirtualRange(startInMapping, limitInMapping);

		if(!success)
			co_return progress;
	}

	co_return progress;
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

void AddressSpace::activate(smarter::shared_ptr<AddressSpace, BindableHandle> space) {
	auto pageSpace = &space->pageSpace_;
	PageSpace::activate(smarter::shared_ptr<PageSpace>{space->selfPtr.lock(), pageSpace});
}

AddressSpace::AddressSpace()
: VirtualSpace{&ops_}, ops_{this} { }

AddressSpace::~AddressSpace() { }

void AddressSpace::dispose(BindableHandle) {
	retire();
}

// --------------------------------------------------------
// MemoryViewLockHandle.
// --------------------------------------------------------

MemoryViewLockHandle::~MemoryViewLockHandle() {
	if(_active)
		_view->unlockRange(_offset, _size);
}

// --------------------------------------------------------
// AddressSpaceLockHandle
// --------------------------------------------------------

AddressSpaceLockHandle::AddressSpaceLockHandle(smarter::shared_ptr<AddressSpace, BindableHandle> space,
		void *pointer, size_t length)
: _space{std::move(space)}, _address{reinterpret_cast<uintptr_t>(pointer)}, _length{length} {
	if(!_length)
		return;
	assert(_address);

	// TODO: Verify the mapping's size.
	_mapping = _space->getMapping(_address);
	assert(_mapping);
}

AddressSpaceLockHandle::~AddressSpaceLockHandle() {
	if(!_length)
		return;

	if(_active)
		_mapping->unlockVirtualRange(_address - _mapping->address, _length);
}

bool AddressSpaceLockHandle::acquire(smarter::shared_ptr<WorkQueue> wq, AcquireNode *node) {
	if(!_length) {
		_active = true;
		return true;
	}

	async::detach_with_allocator(*kernelAlloc, [] (AddressSpaceLockHandle *self,
			smarter::shared_ptr<WorkQueue> wq, AcquireNode *node) -> coroutine<void> {
		auto misalign = self->_address & (kPageSize - 1);
		auto lockOutcome = co_await self->_mapping->lockVirtualRange(
				(self->_address - self->_mapping->address) & ~(kPageSize - 1),
				(self->_length + misalign + kPageSize - 1) & ~(kPageSize - 1), wq);
		assert(lockOutcome);
		auto populateOutcome = co_await self->_mapping->populateVirtualRange(
				(self->_address - self->_mapping->address) & ~(kPageSize - 1),
				(self->_length + misalign + kPageSize - 1) & ~(kPageSize - 1), wq);
		assert(populateOutcome);

		self->_active = true;
		node->complete();
	}(this, std::move(wq), node));

	return false;
}

PhysicalAddr AddressSpaceLockHandle::getPhysical(size_t offset) {
	assert(_active);
	assert(offset < _length);

	return _resolvePhysical(_address + offset);
}

void AddressSpaceLockHandle::load(size_t offset, void *pointer, size_t size) {
	assert(_active);
	assert(offset + size <= _length);

	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = (VirtualAddr)_address + offset + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = _resolvePhysical(write - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy((char *)pointer + progress, (char *)accessor.get() + misalign, chunk);
		progress += chunk;
	}
}

Error AddressSpaceLockHandle::write(size_t offset, const void *pointer, size_t size) {
	assert(_active);
	assert(offset + size <= _length);

	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = (VirtualAddr)_address + offset + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = _resolvePhysical(write - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy((char *)accessor.get() + misalign, (char *)pointer + progress, chunk);
		progress += chunk;
	}

	return Error::success;
}

PhysicalAddr AddressSpaceLockHandle::_resolvePhysical(VirtualAddr vaddr) {
	auto range = _mapping->resolveRange(vaddr - _mapping->address);
	return range.get<0>();
}

// --------------------------------------------------------
// NamedMemoryViewLock.
// --------------------------------------------------------

NamedMemoryViewLock::~NamedMemoryViewLock() { }

} // namespace thor

