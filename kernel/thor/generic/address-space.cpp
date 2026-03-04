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

	[[maybe_unused]]
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

// --------------------------------------------------------
// Generic VirtualOperation implementation.
// --------------------------------------------------------

size_t VirtualOperations::getRss() {
	// Derived classes should track RSS; the generic implementaton does not.
	// TODO: As soon as all derived classes implement this, we should make it pure virtual.
	return 0;
}

// --------------------------------------------------------

MemorySlice::MemorySlice(smarter::shared_ptr<MemoryView> view,
		ptrdiff_t view_offset, size_t view_size, CachingFlags cachingFlags)
: _view{std::move(view)}, _viewOffset{view_offset}, _viewSize{view_size}, cachingFlags_{cachingFlags} {
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
	//debugLogger() << "thor: Mapping is destructed" << frg::endlog;
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

uint32_t Mapping::compilePageFlags() {
	uint32_t pageFlags = 0;
	if(flags & MappingFlags::protRead)
		pageFlags |= page_access::read;
	if(flags & MappingFlags::protWrite)
		pageFlags |= page_access::write;
	if(flags & MappingFlags::protExecute)
		pageFlags |= page_access::execute;
	return pageFlags;
}

coroutine<void> Mapping::runEvictionLoop() {
	assert(currentIpl() == ipl::exceptionalWork);

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

		co_await exposeRcu.barrier();

		bool pagesAffected;
		{
			LocalRcuEngine::Guard revokeGuard{revokeRcu};

			// Unmap the memory range.
			auto unmapOutcome = owner->_ops->unmapPages(address + shootOffset, shootSize);
			assert(unmapOutcome);
			pagesAffected = unmapOutcome.value();

			if(pagesAffected)
				co_await owner->_ops->shootdown(address + shootOffset, shootSize);
		}
		if(!pagesAffected)
			co_await revokeRcu.barrier();

		eviction.done();
	}

	evictionDoneEvent.raise();
}

// --------------------------------------------------------
// CowMapping
// --------------------------------------------------------

CowChain::CowChain()
: _pages{*kernelAlloc} {
}

CowChain::~CowChain() {
	if(logCleanup)
		infoLogger() << "thor: Releasing CowChain" << frg::endlog;
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
		debugLogger() << "thor: VirtualSpace is destructed" << frg::endlog;

	while(_holes.get_root()) {
		auto hole = _holes.get_root();
		_holes.remove(hole);
		frg::destruct(*kernelAlloc, hole);
	}
}

void VirtualSpace::retire() {
	if(logCleanup)
		debugLogger() << "thor: VirtualSpace is cleared" << frg::endlog;

	// TODO: It would be less ugly to run this in a non-detached way.
	spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), [] (smarter::shared_ptr<VirtualSpace> self)
			-> coroutine<void> {
		co_await self->_consistencyMutex.async_lock();
		frg::unique_lock consistencyLock{frg::adopt_lock, self->_consistencyMutex};

		auto mapping = self->_mappings.first();
		while(mapping) {
			assert(mapping->state == MappingState::active);
			mapping->state = MappingState::zombie;

			co_await mapping->exposeRcu.barrier();

			auto unmapOutcome = self->_ops->unmapPages(mapping->address, mapping->length);
			assert(unmapOutcome);

			mapping = MappingTree::successor(mapping);
		}

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

coroutine<frg::expected<Error, VirtualAddr>>
VirtualSpace::map(smarter::borrowed_ptr<MemorySlice> slice,
		VirtualAddr address, size_t offset, size_t length, uint32_t flags) {
	assert(currentIpl() == ipl::exceptionalWork);
	assert(length);
	assert(!(length % kPageSize));

	if(offset + length > slice->length())
		co_return Error::bufferTooSmall;

	co_await _consistencyMutex.async_lock();
	frg::unique_lock consistencyLock{frg::adopt_lock, _consistencyMutex};

	if (flags & kMapFixed) {
		auto [start, end] = co_await _splitMappings(address, length);
		assert(start || (!start && !end));
		co_await _unmapMappings(address, length, start, end);
	}

	// The shared_ptr to the new Mapping needs to survive until the locks are released.
	VirtualAddr actualAddress;
	smarter::shared_ptr<Mapping> mapping;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto spaceLock = frg::guard(&_snapshotMutex);

		assert((address % kPageSize) == 0);
		if(flags & kMapFixed) {
			actualAddress = FRG_CO_TRY(_allocateAt(address, length));
		}else if(flags & kMapFixedNoReplace) {
			if(_areMappingsInRange(address, length)) {
				co_return Error::alreadyExists;
			}
			actualAddress = FRG_CO_TRY(_allocateAt(address, length));
		}else{
			if(address && !_areMappingsInRange(address, length)) {
				if(auto res = _allocateAt(address, length)) {
					actualAddress = res.unwrap();
				}else {
					actualAddress = FRG_CO_TRY(_allocate(length, flags));
				}
			}else {
				actualAddress = FRG_CO_TRY(_allocate(length, flags));
			}
		}

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

		auto caching = CachingMode::null;
		if(slice->getCachingFlags() == cacheWriteCombine)
			caching = CachingMode::writeCombine;

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
		if((mappingFlags & MappingFlags::permissionMask) & MappingFlags::protRead)
			pageFlags |= page_access::read;

		LocalRcuEngine::Guard exposeGuard{mapping->exposeRcu};

		auto mapOutcome = _ops->mapPresentPages(mapping->address, mapping->view.get(),
				mapping->viewOffset, mapping->length, pageFlags, caching);
		assert(mapOutcome);
	}

	if(mapping->view->canEvictMemory())
		spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), mapping->runEvictionLoop());

	co_return actualAddress;
}

coroutine<frg::expected<Error>>
VirtualSpace::protect(VirtualAddr address, size_t length, uint32_t flags) {
	assert(currentIpl() == ipl::exceptionalWork);

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

	co_await _consistencyMutex.async_lock();
	frg::unique_lock consistencyLock{frg::adopt_lock, _consistencyMutex};

	auto [start, end] = co_await _splitMappings(address, length);
	assert(start || (!start && !end));
	for (auto it = start; it != end;) {
		auto mapping = it->selfPtr.lock();
		it = MappingTree::successor(it);

		mapping->protect(static_cast<MappingFlags>(mappingFlags));

		assert(mapping->state == MappingState::active);

		uint32_t pageFlags = 0;
		if((mapping->flags & MappingFlags::permissionMask) & MappingFlags::protWrite)
			pageFlags |= page_access::write;
		if((mapping->flags & MappingFlags::permissionMask) & MappingFlags::protExecute)
			pageFlags |= page_access::execute;
		if((mapping->flags & MappingFlags::permissionMask) & MappingFlags::protRead)
			pageFlags |= page_access::read;

		auto caching = CachingMode::null;
		if(mapping->slice->getCachingFlags() == cacheWriteCombine)
			caching = CachingMode::writeCombine;

		co_await mapping->exposeRcu.barrier();

		bool pagesAffected;
		{
			LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

			auto restrictOutcome = _ops->restrictPages(mapping->address,
					mapping->length, pageFlags, caching);
			assert(restrictOutcome);
			pagesAffected = restrictOutcome.value();

			if(pagesAffected)
				co_await _ops->shootdown(mapping->address, mapping->length);
		}
		if(!pagesAffected)
			co_await mapping->revokeRcu.barrier();
	}

	co_return {};
}

coroutine<frg::expected<Error>> VirtualSpace::unmap(VirtualAddr address, size_t length) {
	assert(currentIpl() == ipl::exceptionalWork);

	co_await _consistencyMutex.async_lock();
	frg::unique_lock consistencyLock{frg::adopt_lock, _consistencyMutex};

	auto [start, end] = co_await _splitMappings(address, length);
	assert(start || (!start && !end));
	co_await _unmapMappings(address, length, start, end);

	co_return {};
}

coroutine<frg::expected<Error>>
VirtualSpace::synchronize(VirtualAddr address, size_t size) {
	assert(currentIpl() == ipl::exceptionalWork);

	co_await _consistencyMutex.async_lock_shared();
	frg::shared_lock consistencyLock{frg::adopt_lock, _consistencyMutex};

	auto misalign = address & (kPageSize - 1);
	auto alignedAddress = address & ~(kPageSize - 1);
	auto alignedSize = (size + misalign + kPageSize - 1) & ~(kPageSize - 1);

	size_t overallProgress = 0;
	while(overallProgress < alignedSize) {
		smarter::shared_ptr<Mapping> mapping;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto spaceGuard = frg::guard(&_snapshotMutex);

			mapping = _findMapping(alignedAddress + overallProgress);
		}
		assert(mapping);

		auto mappingOffset = alignedAddress + overallProgress - mapping->address;
		auto mappingChunk = frg::min(alignedSize - overallProgress,
				mapping->length - mappingOffset);
		assert(mapping->state == MappingState::active);
		assert(mappingOffset + mappingChunk <= mapping->length);

		auto cleanOutcome = _ops->cleanPages(mapping->address + mappingOffset, mappingChunk);
		assert(cleanOutcome);

		overallProgress += mappingChunk;
	}
	co_await _ops->shootdown(alignedAddress, alignedSize);

	co_return {};
}

coroutine<frg::expected<Error>>
VirtualSpace::handleFault(VirtualAddr address, uint32_t faultFlags) {
	assert(currentIpl() == ipl::exceptionalWork);

	co_await _consistencyMutex.async_lock_shared();
	frg::shared_lock consistencyLock{frg::adopt_lock, _consistencyMutex};

	smarter::shared_ptr<Mapping> mapping;
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto space_guard = frg::guard(&_snapshotMutex);

		mapping = _findMapping(address);
	}
	if(!mapping)
		co_return Error::fault;

	// Check access attributes.
	if((faultFlags & VirtualSpace::kFaultWrite)
			&& !((mapping->flags & MappingFlags::protWrite)))
		co_return Error::badPermissions;
	if((faultFlags & VirtualSpace::kFaultExecute)
			&& !((mapping->flags & MappingFlags::protExecute)))
		co_return Error::badPermissions;

	// TODO: Aligning should not be necessary here.
	auto offset = (address - mapping->address) & ~(kPageSize - 1);

	while(true) {
		FetchFlags fetchFlags = 0;
		if(mapping->flags & MappingFlags::dontRequireBacking)
			fetchFlags |= fetchDisallowBacking;
		if(faultFlags & VirtualSpace::kFaultWrite)
			fetchFlags |= fetchRequireMutable;

		FRG_CO_TRY(co_await mapping->view->touchRange(
				mapping->viewOffset + offset, kPageSize, fetchFlags));

		auto caching = CachingMode::null;
		if(mapping->slice->getCachingFlags() == cacheWriteCombine)
			caching = CachingMode::writeCombine;

		LocalRcuEngine::Guard exposeGuard{mapping->exposeRcu};

		auto remapOutcome = _ops->faultPage(address & ~(kPageSize - 1),
				mapping->view.get(), mapping->viewOffset + offset,
				fetchFlags,
				mapping->compilePageFlags(), caching);
		if(!remapOutcome) {
			if(remapOutcome.error() == Error::spuriousOperation) {
				// Spurious page faults are the result of race conditions.
				// They should be rare. If they happen too often, something is probably wrong!
				warningLogger() << "thor: Spurious page fault" << frg::endlog;
			}else{
				assert(remapOutcome.error() == Error::fault);
				warningLogger() << "thor: Page still not available after touchRange()"
					<< frg::endlog;
				continue;
			}
		}

		co_return {};
	}
}

coroutine<frg::expected<Error, PhysicalAddr>>
VirtualSpace::retrievePhysical(VirtualAddr address) {
	// We do not take _consistencyMutex here since we are only interested in a snapshot.

	smarter::shared_ptr<Mapping> mapping;
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto space_guard = frg::guard(&_snapshotMutex);

		mapping = _findMapping(address);
	}
	if(!mapping)
		co_return Error::fault;

	// TODO: Aligning should not be necessary here.
	auto offset = (address - mapping->address) & ~(kPageSize - 1);

	while(true) {
		FetchFlags fetchFlags = fetchRequireMutable;
		if(mapping->flags & MappingFlags::dontRequireBacking)
			fetchFlags |= fetchDisallowBacking;

		FRG_CO_TRY(co_await mapping->view->touchRange(
				mapping->viewOffset + offset, kPageSize, fetchFlags));

		LocalRcuEngine::Guard exposeGuard{mapping->exposeRcu};
		auto physicalRange = mapping->view->peekRange(mapping->viewOffset + offset, fetchFlags);
		if(physicalRange.physical == PhysicalAddr(-1)) {
			warningLogger() << "thor: Page still not available after touchRange()" << frg::endlog;
			continue;
		}

		co_return physicalRange.physical;
	}
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

bool VirtualSpace::_areMappingsInRange(VirtualAddr address, size_t length) {
	auto end = address + length;

	auto current = _mappings.get_root();
	while(current) {
		auto currentEnd = current->address + current->length;
		if(address < currentEnd && current->address < end) {
			return true;
		}

		if(address < current->address) {
			current = MappingTree::get_left(current);
		}else if(address > current->address) {
			current = MappingTree::get_right(current);
		}else {
			return true;
		}
	}
	return false;
}

frg::expected<Error, VirtualAddr> VirtualSpace::_allocate(size_t length, MapFlags flags) {
	assert(length > 0);
	assert((length % kPageSize) == 0);
//	infoLogger() << "Allocate virtual memory area"
//			<< ", size: 0x" << frg::hex_fmt(length) << frg::endlog;

	if(_holes.get_root()->largestHole < length)
		return Error::noMemory;

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

frg::expected<Error, VirtualAddr> VirtualSpace::_allocateAt(VirtualAddr address, size_t length) {
	assert(!(address % kPageSize));
	assert(!(length % kPageSize));

	auto current = _holes.get_root();
	while(true) {
		if(!current) {
			return Error::noMemory;
		}

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

	if(address - current->address() + length > current->length()) {
		return Error::noMemory;
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

coroutine<frg::tuple<Mapping *, Mapping *>> VirtualSpace::_splitMappings(uintptr_t address, size_t size) {
	// _consistencyMutex is held here by the caller

	auto left = _mappings.get_root();
	while (left) {
		if (auto next = MappingTree::get_left(left))
			left = next;
		else
			break;
	}

	Mapping *start = nullptr, *end = nullptr;
	for (auto it = left; it;) {
		if ((it->address + it->length) <= address) {
			it = MappingTree::successor(it);
			start = it;
			continue;
		}

		if (it->address >= (address + size)) {
			// If no mapping fell into the range, don't set the end iterator
			if (start)
				end = it;

			break;
		}

		if (!start)
			start = it;

		auto mapping = it->selfPtr.lock();
		auto at = address;

		// Starting split not within mapping, consider end split
		if (at <= mapping->address)
			at = address + size;

		if (at > mapping->address && at < (mapping->address + mapping->length)) {
			// Split mapping into left and right part
			smarter::shared_ptr<Mapping> leftMapping = nullptr;
			smarter::shared_ptr<Mapping> rightMapping = nullptr;

			assert(mapping->state == MappingState::active);
			mapping->state = MappingState::zombie;

			{
				auto leftSize = at - mapping->address;
				leftMapping = smarter::allocate_shared<Mapping>(Allocator{},
						leftSize, mapping->flags, mapping->slice,
						mapping->viewOffset);
				leftMapping->selfPtr = leftMapping;

				leftMapping->tie(selfPtr.lock(), mapping->address);
			}

			{
				auto rightOffset = at - mapping->address;
				rightMapping = smarter::allocate_shared<Mapping>(Allocator{},
						mapping->length - rightOffset, mapping->flags, mapping->slice,
						mapping->viewOffset + rightOffset);
				rightMapping->selfPtr = rightMapping;

				rightMapping->tie(selfPtr.lock(), at);
			}

			assert(leftMapping && rightMapping);

			// Now remove the mapping and insert the new mappings.
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_snapshotMutex);

				_mappings.remove(mapping.get());

				_mappings.insert(leftMapping.get());
				assert(leftMapping->state == MappingState::null);
				leftMapping->state = MappingState::active;

				_mappings.insert(rightMapping.get());
				assert(rightMapping->state == MappingState::null);
				rightMapping->state = MappingState::active;
			}

			// Retire the old mapping and start using the new ones.
			// We keep one reference until the detach the observer.
			leftMapping.ctr()->increment();
			leftMapping->view->addObserver(&leftMapping->observer);
			if (leftMapping->view->canEvictMemory())
				spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), leftMapping->runEvictionLoop());

			// We keep one reference until the detach the observer.
			rightMapping.ctr()->increment();
			rightMapping->view->addObserver(&rightMapping->observer);
			if (rightMapping->view->canEvictMemory())
				spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), rightMapping->runEvictionLoop());

			assert(mapping->state == MappingState::zombie);
			mapping->state = MappingState::retired;

			if (mapping->view->canEvictMemory()) {
				mapping->cancelEviction.cancel();
				co_await mapping->evictionDoneEvent.wait();
			}
			mapping->view->removeObserver(&mapping->observer);
			mapping->selfPtr.ctr()->decrement();

			// If start pointed to the freshly-removed mapping,
			// determine the correct mapping to use as our new start.
			// Generally, this will be the left mapping, however, if we split the mapping 3 ways
			// so as to choose the center one, this will be the right mapping.
			if (start == it) {
				if(address < at) {
					start = leftMapping.get();
				} else {
					start = rightMapping.get();
				}
			}

			it = rightMapping.get();
		} else {
			it = MappingTree::successor(it);
		}
	}

	co_return frg::make_tuple(start, end);
}

coroutine<void> VirtualSpace::_unmapMappings(VirtualAddr address, size_t length, Mapping *start, Mapping *end) {
	for (auto it = start; it != end;) {
		auto mapping = it->selfPtr.lock();
		it = MappingTree::successor(it);

		if (mapping->address >= address && (mapping->address + mapping->length) <= (address + length)) {
			assert(mapping->state == MappingState::active);
			mapping->state = MappingState::zombie;

			co_await mapping->exposeRcu.barrier();

			bool pagesAffected;
			{
				LocalRcuEngine::Guard revokeGuard{mapping->revokeRcu};

				// Mark pages as dirty and unmap without holding a lock.
				auto unmapOutcome = _ops->unmapPages(mapping->address, mapping->length);
				assert(unmapOutcome);
				pagesAffected = unmapOutcome.value();

				if(pagesAffected)
					co_await _ops->shootdown(mapping->address, mapping->length);
			}
			if(!pagesAffected)
				co_await mapping->revokeRcu.barrier();

			_mappings.remove(mapping.get());

			assert(mapping->state == MappingState::zombie);
			mapping->state = MappingState::retired;

			if(mapping->view->canEvictMemory()) {
				mapping->cancelEviction.cancel();
				co_await mapping->evictionDoneEvent.wait();
			}
			mapping->view->removeObserver(&mapping->observer);
			mapping->selfPtr.ctr()->decrement();

			// Finally, coalesce the hole in the hole tree.

			// Find the holes that preceede/succeede mapping.
			Hole *pre;
			Hole *succ;

			auto current = _holes.get_root();
			while(true) {
				assert(current);
				if(mapping->address < current->address()) {
					if(HoleTree::get_left(current)) {
						current = HoleTree::get_left(current);
					}else{
						pre = HoleTree::predecessor(current);
						succ = current;
						break;
					}
				}else{
					assert(mapping->address >= current->address() + current->length());
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
			if(pre && pre->address() + pre->length() == mapping->address
					&& succ && mapping->address + mapping->length == succ->address()) {
				auto hole = frg::construct<Hole>(*kernelAlloc, pre->address(),
						pre->length() + mapping->length + succ->length());

				_holes.remove(pre);
				_holes.remove(succ);
				_holes.insert(hole);
				frg::destruct(*kernelAlloc, pre);
				frg::destruct(*kernelAlloc, succ);
			}else if(pre && pre->address() + pre->length() == mapping->address) {
				auto hole = frg::construct<Hole>(*kernelAlloc,
						pre->address(), pre->length() + mapping->length);

				_holes.remove(pre);
				_holes.insert(hole);
				frg::destruct(*kernelAlloc, pre);
			}else if(succ && mapping->address + mapping->length == succ->address()) {
				auto hole = frg::construct<Hole>(*kernelAlloc,
						mapping->address, mapping->length + succ->length());

				_holes.remove(succ);
				_holes.insert(hole);
				frg::destruct(*kernelAlloc, succ);
			}else{
				auto hole = frg::construct<Hole>(*kernelAlloc,
						mapping->address, mapping->length);

				_holes.insert(hole);
			}
		}
	}

	co_return;
}

coroutine<size_t> VirtualSpace::readPartialSpace(uintptr_t address,
		void *buffer, size_t size) {
	assert(currentIpl() == ipl::exceptionalWork);

	// We do not take _consistencyMutex here since we are only interested in a snapshot.

	size_t progress = 0;
	while(progress < size) {
		smarter::shared_ptr<Mapping> mapping;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto spaceGuard = frg::guard(&_snapshotMutex);

			mapping = _findMapping(address + progress);
		}
		if(!mapping)
			co_return progress;

		auto startInMapping = address + progress - mapping->address;
		auto limitInMapping = frg::min(size - progress, mapping->length - startInMapping);
		// Otherwise, _findMapping() would have returned garbage.
		assert(limitInMapping);

		FetchFlags fetchFlags = 0;
		if(mapping->flags & MappingFlags::dontRequireBacking)
			fetchFlags |= fetchDisallowBacking;

		auto copyOutcome = co_await mapping->view->copyFrom(
				mapping->viewOffset + startInMapping,
				reinterpret_cast<std::byte *>(buffer) + progress,
				limitInMapping, fetchFlags);
		if(!copyOutcome)
			co_return progress;

		progress += limitInMapping;
	}
	co_return progress;
}

coroutine<size_t> VirtualSpace::writePartialSpace(uintptr_t address,
		const void *buffer, size_t size) {
	assert(currentIpl() == ipl::exceptionalWork);

	// We do not take _consistencyMutex here since we are only interested in a snapshot.

	size_t progress = 0;
	while(progress < size) {
		smarter::shared_ptr<Mapping> mapping;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto spaceGuard = frg::guard(&_snapshotMutex);

			mapping = _findMapping(address + progress);
		}
		if(!mapping)
			co_return progress;

		auto startInMapping = address + progress - mapping->address;
		auto limitInMapping = frg::min(size - progress, mapping->length - startInMapping);
		// Otherwise, _findMapping() would have returned garbage.
		assert(limitInMapping);

		FetchFlags fetchFlags = 0;
		if(mapping->flags & MappingFlags::dontRequireBacking)
			fetchFlags |= fetchDisallowBacking;

		auto copyOutcome = co_await mapping->view->copyTo(
				mapping->viewOffset + startInMapping,
				reinterpret_cast<const std::byte *>(buffer) + progress,
				limitInMapping, fetchFlags);
		if(!copyOutcome)
			co_return progress;

		progress += limitInMapping;
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
// NamedMemoryViewLock.
// --------------------------------------------------------

NamedMemoryViewLock::~NamedMemoryViewLock() { }

} // namespace thor
