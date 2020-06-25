
#include <type_traits>
#include "execution/coroutine.hpp"
#include "kernel.hpp"
#include "fiber.hpp"
#include "service_helpers.hpp"
#include <frg/container_of.hpp>
#include "types.hpp"

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
		frigg::infoLogger() << "thor: RSS of " << space << " increases above "
				<< (rss / 1024) << " KiB" << frigg::endLog;
		frigg::infoLogger() << "thor:     Physical usage: "
				<< (physicalAllocator->numUsedPages() * 4) << " KiB, kernel usage: "
				<< (kernelMemoryUsage / 1024) << " KiB" << frigg::endLog;
	}
}

MemorySlice::MemorySlice(frigg::SharedPtr<MemoryView> view,
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
		frigg::infoLogger() << "largestHole violation: " << "Expected " << size
				<< ", got " << hole->largestHole << "." << frigg::endLog;
		return false;
	}

	// Check non-overlapping memory areas invariant.
	if(pred && hole->address() < pred->address() + pred->length()) {
		frigg::infoLogger() << "Non-overlapping (left) violation" << frigg::endLog;
		return false;
	}
	if(succ && hole->address() + hole->length() > succ->address()) {
		frigg::infoLogger() << "Non-overlapping (right) violation" << frigg::endLog;
		return false;
	}

	return true;
}

// --------------------------------------------------------
// Mapping
// --------------------------------------------------------

Mapping::Mapping(size_t length, MappingFlags flags,
		frigg::SharedPtr<MemorySlice> slice_, uintptr_t viewOffset)
: length{length}, flags{flags},
		slice{std::move(slice_)}, viewOffset{viewOffset} {
	assert(viewOffset >= slice->offset());
	assert(viewOffset + length <= slice->offset() + slice->length());
	view = slice->getView();
}

Mapping::~Mapping() {
	assert(state == MappingState::retired);
	//frigg::infoLogger() << "\e[31mthor: Mapping is destructed\e[39m" << frigg::endLog;
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
		async::any_receiver<frg::expected<Error>> receiver) {
	async::detach_with_allocator(*kernelAlloc, [] (Mapping *self,
			uintptr_t offset, size_t size,
			async::any_receiver<frg::expected<Error>> receiver) -> coroutine<void> {
		size_t progress = 0;
		while(progress < size) {
			auto outcome = co_await self->touchVirtualPage(offset + progress);
			if(!outcome) {
				async::execution::set_value(receiver, outcome.error());
				co_return;
			}
			progress += outcome.value().range.get<1>();
		}
		async::execution::set_value(receiver, frg::success);
	}(this, offset, size, std::move(receiver)));
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
		async::any_receiver<frg::expected<Error>> receiver) {
	// This can be removed if we change the return type of asyncLockRange to frg::expected.
	auto transformError = [] (Error e) -> frg::expected<Error> {
		if(!e)
			return {};
		return e;
	};
	async::spawn_with_allocator(*kernelAlloc,
			async::transform(view->asyncLockRange(viewOffset + offset, size), transformError),
			std::move(receiver));
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
		async::any_receiver<frg::expected<Error, TouchVirtualResult>> receiver) {
	assert(state == MappingState::active);

	async::detach_with_allocator(*kernelAlloc, [] (Mapping *self, uintptr_t offset,
			async::any_receiver<frg::expected<Error, TouchVirtualResult>> receiver)
			-> coroutine<void> {
		FetchFlags fetchFlags = 0;
		if(self->flags & MappingFlags::dontRequireBacking)
			fetchFlags |= FetchNode::disallowBacking;

		if(auto e = co_await self->view->asyncLockRange(
				(self->viewOffset + offset) & ~(kPageSize - 1), kPageSize); e)
			assert(!"asyncLockRange() failed");

		auto [error, range, rangeFlags] = co_await self->view->fetchRange(
				self->viewOffset + offset);

		// TODO: Update RSS, handle dirty pages, etc.
		auto pageOffset = self->address + offset;
		self->owner->_ops->unmapSingle4k(pageOffset & ~(kPageSize - 1));
		self->owner->_ops->mapSingle4k(pageOffset & ~(kPageSize - 1),
				range.get<0>() & ~(kPageSize - 1),
				self->compilePageFlags(), range.get<2>());
		self->owner->_residuentSize += kPageSize;
		logRss(self->owner.get());

		self->view->unlockRange((self->viewOffset + offset) & ~(kPageSize - 1), kPageSize);
		async::execution::set_value(receiver, TouchVirtualResult{range, false});
	}(this, offset, std::move(receiver)));
}

void Mapping::install() {
	assert(state == MappingState::null);
	state = MappingState::active;

	view->addObserver(&observer);

	if(view->canEvictMemory())
		async::detach_with_allocator(*kernelAlloc, runEvictionLoop());

	uint32_t pageFlags = 0;
	if((flags & MappingFlags::permissionMask) & MappingFlags::protWrite)
		pageFlags |= page_access::write;
	if((flags & MappingFlags::permissionMask) & MappingFlags::protExecute)
		pageFlags |= page_access::execute;
	// TODO: Allow inaccessible mappings.
	assert((flags & MappingFlags::permissionMask) & MappingFlags::protRead);

	// Synchronize with the eviction loop.
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&evictMutex);

	for(size_t progress = 0; progress < length; progress += kPageSize) {
		auto physicalRange = view->peekRange(viewOffset + progress);

		VirtualAddr vaddr = address + progress;
		assert(!owner->_ops->isMapped(vaddr));
		if(physicalRange.get<0>() != PhysicalAddr(-1)) {
			owner->_ops->mapSingle4k(vaddr, physicalRange.get<0>(),
					pageFlags, physicalRange.get<1>());
			owner->_residuentSize += kPageSize;
			logRss(owner.get());
		}
	}
}

void Mapping::reinstall() {
	assert(state == MappingState::active);

	uint32_t pageFlags = 0;
	if((flags & MappingFlags::permissionMask) & MappingFlags::protWrite)
		pageFlags |= page_access::write;
	if((flags & MappingFlags::permissionMask) & MappingFlags::protExecute)
		pageFlags |= page_access::execute;
	// TODO: Allow inaccessible mappings.
	assert((flags & MappingFlags::permissionMask) & MappingFlags::protRead);

	// Synchronize with the eviction loop.
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&evictMutex);

	for(size_t progress = 0; progress < length; progress += kPageSize) {
		auto physicalRange = view->peekRange(viewOffset + progress);

		VirtualAddr vaddr = address + progress;
		auto status = owner->_ops->unmapSingle4k(vaddr);
		if(!(status & page_status::present))
			continue;
		if(status & page_status::dirty)
			view->markDirty(viewOffset + progress, kPageSize);
		if(physicalRange.get<0>() != PhysicalAddr(-1)) {
			owner->_ops->mapSingle4k(vaddr, physicalRange.get<0>(),
					pageFlags, physicalRange.get<1>());
		}else{
			owner->_residuentSize -= kPageSize;
		}
	}
}

void Mapping::synchronize(uintptr_t offset, size_t size) {
	assert(state == MappingState::active);
	assert(offset + size <= length);

	// Synchronize with the eviction loop.
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&evictMutex);

	for(size_t progress = 0; progress < size; progress += kPageSize) {
		VirtualAddr vaddr = address + offset + progress;
		auto status = owner->_ops->cleanSingle4k(vaddr);
		if(!(status & page_status::present))
			continue;
		if(status & page_status::dirty)
			view->markDirty(viewOffset + progress, kPageSize);
	}
}

void Mapping::uninstall() {
	assert(state == MappingState::active);
	state = MappingState::zombie;

	for(size_t progress = 0; progress < length; progress += kPageSize) {
		VirtualAddr vaddr = address + progress;
		auto status = owner->_ops->unmapSingle4k(vaddr);
		if(!(status & page_status::present))
			continue;
		if(status & page_status::dirty)
			view->markDirty(viewOffset + progress, kPageSize);
		owner->_residuentSize -= kPageSize;
	}
}

void Mapping::retire() {
	assert(state == MappingState::zombie);
	state = MappingState::retired;

	if(view->canEvictMemory())
		cancelEviction.cancel();

	// TODO: It would be less ugly to run this in a non-detached way.
	auto cleanUpObserver = [] (Mapping *self) -> coroutine<void> {
		if(self->view->canEvictMemory())
			co_await self->evictionDoneEvent.wait();
		self->view->removeObserver(&self->observer);
		self->selfPtr.ctr()->decrement();
	};
	selfPtr.ctr()->increment(); // Keep this object alive until the coroutines completes.
	async::detach_with_allocator(*kernelAlloc, cleanUpObserver(this));
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

		// Wait until we are allowed to evict existing pages.
		// TODO: invent a more specialized synchronization mechanism for this.
		{
			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&evictMutex);
		}

		// TODO: Perform proper locking here!

		// Unmap the memory range.
		for(size_t pg = 0; pg < shootSize; pg += kPageSize) {
			auto status = owner->_ops->unmapSingle4k(address + shootOffset + pg);
			if(!(status & page_status::present))
				continue;
			if(status & page_status::dirty)
				view->markDirty(viewOffset + shootOffset + pg, kPageSize);
			owner->_residuentSize -= kPageSize;
		}

		// Perform shootdown.
		struct Closure {
			smarter::shared_ptr<Mapping> mapping; // Need to keep the Mapping alive.
			Worklet worklet;
			ShootNode node;
			Eviction eviction;
		} *closure = frigg::construct<Closure>(*kernelAlloc);

		closure->worklet.setup([] (Worklet *base) {
			auto closure = frg::container_of(base, &Closure::worklet);
			closure->eviction.done();
			frigg::destruct(*kernelAlloc, closure);
		});
		closure->mapping = selfPtr.lock();
		closure->eviction = std::move(eviction);

		closure->node.address = address + shootOffset;
		closure->node.size = shootSize;
		closure->node.setup(&closure->worklet);
		if(!owner->_ops->submitShootdown(&closure->node))
			continue;

		closure->eviction.done();
		frigg::destruct(*kernelAlloc, closure);
		continue;
	}

	evictionDoneEvent.raise();
}

// --------------------------------------------------------
// CowMapping
// --------------------------------------------------------

CowChain::CowChain(frigg::SharedPtr<CowChain> chain)
: _superChain{std::move(chain)}, _pages{*kernelAlloc} {
}

CowChain::~CowChain() {
	if(logCleanup)
		frigg::infoLogger() << "thor: Releasing CowChain" << frigg::endLog;

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
	auto hole = frigg::construct<Hole>(*kernelAlloc, address, size);
	_holes.insert(hole);
}

VirtualSpace::~VirtualSpace() {
	if(logCleanup)
		frigg::infoLogger() << "\e[31mthor: VirtualSpace is destructed\e[39m" << frigg::endLog;

	while(_holes.get_root()) {
		auto hole = _holes.get_root();
		_holes.remove(hole);
		frg::destruct(*kernelAlloc, hole);
	}
}

void VirtualSpace::retire() {
	if(logCleanup)
		frigg::infoLogger() << "\e[31mthor: VirtualSpace is cleared\e[39m" << frigg::endLog;

	// TODO: Set some flag to make sure that no mappings are added/deleted.
	auto mapping = _mappings.first();
	while(mapping) {
		mapping->uninstall();
		mapping = MappingTree::successor(mapping);
	}

	struct Closure {
		smarter::shared_ptr<VirtualSpace> self;
		RetireNode retireNode;
		Worklet worklet;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	closure->self = selfPtr.lock();

	closure->retireNode.setup(&closure->worklet);
	closure->worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		auto self = closure->self.get();

		while(self->_mappings.get_root()) {
			auto mapping = self->_mappings.get_root();
			mapping->retire();
			self->_mappings.remove(mapping);
			mapping->selfPtr.ctr()->decrement();
		}

		frg::destruct(*kernelAlloc, closure);
	});
	_ops->retire(&closure->retireNode);
}

smarter::shared_ptr<Mapping> VirtualSpace::getMapping(VirtualAddr address) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto space_guard = frigg::guard(&_mutex);

	return _findMapping(address);
}

Error VirtualSpace::map(frigg::UnsafePtr<MemorySlice> slice, VirtualAddr address,
		size_t offset, size_t length, uint32_t flags, VirtualAddr *actual_address) {
	assert(length);
	assert(!(length % kPageSize));

	if(offset + length > slice->length())
		return kErrBufferTooSmall;

	auto irq_lock = frigg::guard(&irqMutex());
	auto space_guard = frigg::guard(&_mutex);

	VirtualAddr target;
	if(flags & kMapFixed) {
		assert(address);
		assert((address % kPageSize) == 0);
		target = _allocateAt(address, length);
	}else{
		target = _allocate(length, flags);
	}
	assert(target);

//	frigg::infoLogger() << "Creating new mapping at " << (void *)target
//			<< ", length: " << (void *)length << frigg::endLog;

	// Setup a new Mapping object.
	std::underlying_type_t<MappingFlags> mapping_flags = 0;

	// TODO: The upgrading mechanism needs to be arch-specific:
	// Some archs might only support RX, while other support X.
	auto mask = kMapProtRead | kMapProtWrite | kMapProtExecute;
	if((flags & mask) == (kMapProtRead | kMapProtWrite | kMapProtExecute)
			|| (flags & mask) == (kMapProtWrite | kMapProtExecute)) {
		// WX is upgraded to RWX.
		mapping_flags |= MappingFlags::protRead | MappingFlags::protWrite
			| MappingFlags::protExecute;
	}else if((flags & mask) == (kMapProtRead | kMapProtExecute)
			|| (flags & mask) == kMapProtExecute) {
		// X is upgraded to RX.
		mapping_flags |= MappingFlags::protRead | MappingFlags::protExecute;
	}else if((flags & mask) == (kMapProtRead | kMapProtWrite)
			|| (flags & mask) == kMapProtWrite) {
		// W is upgraded to RW.
		mapping_flags |= MappingFlags::protRead | MappingFlags::protWrite;
	}else if((flags & mask) == kMapProtRead) {
		mapping_flags |= MappingFlags::protRead;
	}else{
		assert(!(flags & mask));
	}

	if(flags & kMapDontRequireBacking)
		mapping_flags |= MappingFlags::dontRequireBacking;

	auto mapping = smarter::allocate_shared<Mapping>(Allocator{},
			length, static_cast<MappingFlags>(mapping_flags),
			slice.toShared(), slice->offset() + offset);
	mapping->selfPtr = mapping;

	assert(!(flags & kMapPopulate));

	// Install the new mapping object.
	mapping->tie(selfPtr.lock(), target);
	_mappings.insert(mapping.get());
	mapping->install();
	mapping.release(); // VirtualSpace owns one reference.

	*actual_address = target;
	return kErrSuccess;
}

bool VirtualSpace::protect(VirtualAddr address, size_t length,
		uint32_t flags, AddressProtectNode *node) {
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

	auto irq_lock = frigg::guard(&irqMutex());
	auto space_guard = frigg::guard(&_mutex);

	auto mapping = _findMapping(address);
	assert(mapping);

	// TODO: Allow shrinking of the mapping.
	assert(mapping->address == address);
	assert(mapping->length == length);
	mapping->protect(static_cast<MappingFlags>(mappingFlags));
	mapping->reinstall();

	node->_worklet.setup([] (Worklet *base) {
		auto node = frg::container_of(base, &AddressProtectNode::_worklet);
		node->complete();
	});

	node->_shootNode.address = address;
	node->_shootNode.size = length;
	node->_shootNode.setup(&node->_worklet);
	if(!_ops->submitShootdown(&node->_shootNode))
		return false;
	return true;
}

bool VirtualSpace::unmap(VirtualAddr address, size_t length, AddressUnmapNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto space_guard = frigg::guard(&_mutex);

	auto mapping = _findMapping(address);
	assert(mapping);

	// TODO: Allow shrinking of the mapping.
	assert(mapping->address == address);
	assert(mapping->length == length);
	mapping->uninstall();

	static constexpr auto deleteMapping = [] (VirtualSpace *space, Mapping *mapping) {
		space->_mappings.remove(mapping);
		mapping->retire();
		mapping->selfPtr.ctr()->decrement();
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
			auto hole = frigg::construct<Hole>(*kernelAlloc, pre->address(),
					pre->length() + length + succ->length());

			space->_holes.remove(pre);
			space->_holes.remove(succ);
			space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, pre);
			frigg::destruct(*kernelAlloc, succ);
		}else if(pre && pre->address() + pre->length() == address) {
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					pre->address(), pre->length() + length);

			space->_holes.remove(pre);
			space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, pre);
		}else if(succ && address + length == succ->address()) {
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					address, length + succ->length());

			space->_holes.remove(succ);
			space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, succ);
		}else{
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					address, length);

			space->_holes.insert(hole);
		}
	};

	node->_worklet.setup([] (Worklet *base) {
		auto node = frg::container_of(base, &AddressUnmapNode::_worklet);

		auto irq_lock = frigg::guard(&irqMutex());
		auto space_guard = frigg::guard(&node->_space->_mutex);

		deleteMapping(node->_space, node->_mapping.get());
		closeHole(node->_space, node->_shootNode.address, node->_shootNode.size);
		node->complete();
	});

	node->_space = this;
	node->_mapping = mapping;
	node->_shootNode.address = address;
	node->_shootNode.size = length;
	node->_shootNode.setup(&node->_worklet);
	if(!_ops->submitShootdown(&node->_shootNode))
		return false;

	deleteMapping(this, mapping.get());
	closeHole(this, address, length);
	return true;
}

void VirtualSpace::synchronize(VirtualAddr address, size_t size,
		async::any_receiver<void> receiver) {
	auto misalign = address & (kPageSize - 1);
	auto alignedAddress = address & ~(kPageSize - 1);
	auto alignedSize = (size + misalign + kPageSize - 1) & ~(kPageSize - 1);

	async::detach_with_allocator(*kernelAlloc, [] (VirtualSpace *self, VirtualAddr alignedAddress, size_t alignedSize,
			async::any_receiver<void> receiver) -> coroutine<void> {
		size_t progress = 0;
		while(progress < alignedSize) {
			smarter::shared_ptr<Mapping> mapping;
			{
				auto irqLock = frigg::guard(&irqMutex());
				auto spaceGuard = frigg::guard(&self->_mutex);

				mapping = self->_findMapping(alignedAddress + progress);
			}
			assert(mapping);

			auto offset = alignedAddress + progress - mapping->address;
			auto chunk = frg::min(alignedSize - progress, mapping->length - offset);
			mapping->synchronize(offset, chunk);
			progress += chunk;
		}
		co_await self->_ops->shootdown(alignedAddress, alignedSize);

		receiver.set_value();
	}(this, alignedAddress, alignedSize, std::move(receiver)));
}

bool VirtualSpace::handleFault(VirtualAddr address, uint32_t faultFlags, FaultNode *node) {
	node->_address = address;
	node->_flags = faultFlags;

	smarter::shared_ptr<Mapping> mapping;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		auto space_guard = frigg::guard(&_mutex);

		mapping = _findMapping(address);
		if(!mapping) {
			node->_resolved = false;
			return true;
		}
	}

	node->_mapping = mapping;

	// Here we do the mapping-based fault handling.
	if(node->_flags & VirtualSpace::kFaultWrite)
		if(!((mapping->flags & MappingFlags::permissionMask) & MappingFlags::protWrite)) {
			node->_resolved = false;
			return true;
		}
	if(node->_flags & VirtualSpace::kFaultExecute)
		if(!((mapping->flags & MappingFlags::permissionMask) & MappingFlags::protExecute)) {
			node->_resolved = false;
			return true;
		}

	async::detach_with_allocator(*kernelAlloc, [] (smarter::shared_ptr<Mapping> mapping,
			uintptr_t address, FaultNode *node) -> coroutine<void> {
		auto faultPage = (node->_address - mapping->address) & ~(kPageSize - 1);
		auto outcome = co_await mapping->touchVirtualPage(faultPage);
		if(!outcome) {
			node->_resolved = false;
			WorkQueue::post(node->_handled);
			co_return;
		}

		// Spurious page faults are the result of race conditions.
		// They should be rare. If they happen too often, something is probably wrong!
		if(outcome.value().spurious)
				frigg::infoLogger() << "\e[33m" "thor: Spurious page fault"
						"\e[39m" << frigg::endLog;
		node->_resolved = true;
		WorkQueue::post(node->_handled);
	}(std::move(mapping), address, node));
	return false;
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
//	frigg::infoLogger() << "Allocate virtual memory area"
//			<< ", size: 0x" << frigg::logHex(length) << frigg::endLog;

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
				_splitHole(current, 0, length);
				return current->address();
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
				size_t offset = current->length() - length;
				_splitHole(current, offset, length);
				return current->address() + offset;
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
		auto predecessor = frigg::construct<Hole>(*kernelAlloc, hole->address(), offset);
		_holes.insert(predecessor);
	}

	if(offset + length < hole->length()) {
		auto successor = frigg::construct<Hole>(*kernelAlloc,
				hole->address() + offset + length, hole->length() - (offset + length));
		_holes.insert(successor);
	}

	frigg::destruct(*kernelAlloc, hole);
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

MemoryViewLockHandle::MemoryViewLockHandle(frigg::SharedPtr<MemoryView> view,
		uintptr_t offset, size_t size)
: _view{std::move(view)}, _offset{offset}, _size{size} {
	if(auto e = _view->lockRange(_offset, _size); e)
		return;
	_active = true;
}

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

bool AddressSpaceLockHandle::acquire(AcquireNode *node) {
	if(!_length) {
		_active = true;
		return true;
	}

	async::detach_with_allocator(*kernelAlloc, [] (AddressSpaceLockHandle *self,
			AcquireNode *node) -> coroutine<void> {
		auto misalign = self->_address & (kPageSize - 1);
		auto lockOutcome = co_await self->_mapping->lockVirtualRange(
				(self->_address - self->_mapping->address) & ~(kPageSize - 1),
				(self->_length + misalign + kPageSize - 1) & ~(kPageSize - 1));
		assert(lockOutcome);
		auto populateOutcome = co_await self->_mapping->populateVirtualRange(
				(self->_address - self->_mapping->address) & ~(kPageSize - 1),
				(self->_length + misalign + kPageSize - 1) & ~(kPageSize - 1));
		assert(populateOutcome);
		self->_active = true;
		WorkQueue::post(node->_acquired);
	}(this, node));
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
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

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
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = _resolvePhysical(write - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy((char *)accessor.get() + misalign, (char *)pointer + progress, chunk);
		progress += chunk;
	}

	return kErrSuccess;
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

