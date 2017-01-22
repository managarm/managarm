
#include <type_traits>
#include "kernel.hpp"

namespace thor {

namespace {
	frigg::Tuple<uintptr_t, size_t> alignRange(uintptr_t offset, size_t length, size_t align) {
		auto misalign = offset & (align - 1);
		return frigg::Tuple<uintptr_t, size_t>{offset - misalign,
				(misalign + length + (align - 1)) & ~(align - 1)};
	}
}

// --------------------------------------------------------
// Memory
// --------------------------------------------------------

void Memory::transfer(frigg::UnsafePtr<Memory> dest_memory, uintptr_t dest_offset,
		frigg::UnsafePtr<Memory> src_memory, uintptr_t src_offset, size_t length) {
	dest_memory->acquire(dest_offset, length);
	src_memory->acquire(src_offset, length);

	size_t progress = 0;
	while(progress < length) {
		auto dest_misalign = (dest_offset + progress) % kPageSize;
		auto src_misalign = (src_offset + progress) % kPageSize;
		size_t chunk = frigg::min(frigg::min(kPageSize - dest_misalign,
				kPageSize - src_misalign), length - progress);
		
		PhysicalAddr dest_page = dest_memory->peekRange(dest_offset + progress - dest_misalign);
		PhysicalAddr src_page = src_memory->peekRange(src_offset + progress - dest_misalign);
		assert(dest_page != PhysicalAddr(-1));
		assert(src_page != PhysicalAddr(-1));
		
		PageAccessor dest_accessor{generalWindow, dest_page};
		PageAccessor src_accessor{generalWindow, src_page};
		memcpy((uint8_t *)dest_accessor.get() + dest_misalign,
				(uint8_t *)src_accessor.get() + src_misalign, chunk);
		
		progress += chunk;
	}

	dest_memory->release(dest_offset, length);
	src_memory->release(src_offset, length);
}

size_t Memory::getLength() {
	switch(tag()) {
	case MemoryTag::hardware: return static_cast<HardwareMemory *>(this)->getLength();
	case MemoryTag::allocated: return static_cast<AllocatedMemory *>(this)->getLength();
	case MemoryTag::backing: return static_cast<BackingMemory *>(this)->getLength();
	case MemoryTag::frontal: return static_cast<FrontalMemory *>(this)->getLength();
	default:
		frigg::panicLogger() << "Memory::getLength(): Unexpected tag" << frigg::endLog;
		__builtin_unreachable();
	}
}

void Memory::submitInitiateLoad(frigg::SharedPtr<InitiateBase> initiate) {
	switch(tag()) {
	case MemoryTag::frontal:
		static_cast<FrontalMemory *>(this)->submitInitiateLoad(frigg::move(initiate));
		break;
	case MemoryTag::hardware:
	case MemoryTag::allocated:
		initiate->complete(kErrSuccess);
		break;
	case MemoryTag::copyOnWrite:
		assert(!"Not implemented yet");
	default:
		assert(!"Not supported");
	}
}

void Memory::submitHandleLoad(frigg::SharedPtr<ManageBase> handle) {
	switch(tag()) {
	case MemoryTag::backing:
		static_cast<BackingMemory *>(this)->submitHandleLoad(frigg::move(handle));
		break;
	default:
		assert(!"Not supported");
	}
}

void Memory::completeLoad(size_t offset, size_t length) {
	switch(tag()) {
	case MemoryTag::backing:
		static_cast<BackingMemory *>(this)->completeLoad(offset, length);
		break;
	default:
		assert(!"Not supported");
	}
}

void Memory::load(size_t offset, void *buffer, size_t length) {
	acquire(offset, length);

	size_t progress = 0;
	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, length);
		PhysicalAddr page = peekRange(offset - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{generalWindow, page};
		memcpy(buffer, (uint8_t *)accessor.get() + misalign, prefix);
		progress += prefix;
	}

	while(length - progress >= kPageSize) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = peekRange(offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{generalWindow, page};
		memcpy((uint8_t *)buffer + progress, accessor.get(), kPageSize);
		progress += kPageSize;
	}

	if(length - progress > 0) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = peekRange(offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{generalWindow, page};
		memcpy((uint8_t *)buffer + progress, accessor.get(), length - progress);
	}

	release(offset, length);
}

void Memory::copyFrom(size_t offset, void *buffer, size_t length) {
	acquire(offset, length);

	size_t progress = 0;
	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, length);
		PhysicalAddr page = peekRange(offset - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{generalWindow, page};
		memcpy((uint8_t *)accessor.get() + misalign, buffer, prefix);
		progress += prefix;
	}

	while(length - progress >= kPageSize) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = peekRange(offset + progress);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{generalWindow, page};
		memcpy(accessor.get(), (uint8_t *)buffer + progress, kPageSize);
		progress += kPageSize;
	}

	if(length - progress > 0) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = peekRange(offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{generalWindow, page};
		memcpy(accessor.get(), (uint8_t *)buffer + progress, length - progress);
	}

	release(offset, length);
}

// --------------------------------------------------------
// HardwareMemory
// --------------------------------------------------------

HardwareMemory::HardwareMemory(PhysicalAddr base, size_t length)
: Memory(MemoryTag::hardware), _base(base), _length(length) {
	assert(base % kPageSize == 0);
	assert(length % kPageSize == 0);
}

HardwareMemory::~HardwareMemory() {
	// For now we do nothing when deallocating hardware memory.
}

void HardwareMemory::acquire(uintptr_t offset, size_t length) {
	// Hardware memory is always available.
	(void)offset;
	(void)length;
}

void HardwareMemory::release(uintptr_t offset, size_t length) {
	// Hardware memory is always available.
	(void)offset;
	(void)length;
}

PhysicalAddr HardwareMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	return _base + offset;
}

PhysicalAddr HardwareMemory::fetchRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	return _base + offset;
}

size_t HardwareMemory::getLength() {
	return _length;
}

// --------------------------------------------------------
// AllocatedMemory
// --------------------------------------------------------

AllocatedMemory::AllocatedMemory(size_t length, size_t chunk_size, size_t chunk_align)
: Memory(MemoryTag::allocated), _physicalChunks(*kernelAlloc),
		_chunkSize(chunk_size), _chunkAlign(chunk_align) {
	assert(_chunkSize % kPageSize == 0);
	assert(_chunkAlign % kPageSize == 0);
	assert(_chunkSize % _chunkAlign == 0);
	assert(length % _chunkSize == 0);
	_physicalChunks.resize(length / _chunkSize, PhysicalAddr(-1));
}

AllocatedMemory::~AllocatedMemory() {
	// TODO: This destructor takes a lock. This is potentially unexpected.
	// Rework this to only schedule the deallocation but not actually perform it?
	for(size_t i = 0; i < _physicalChunks.size(); ++i) {
		if(_physicalChunks[i] != PhysicalAddr(-1))
			physicalAllocator->free(_physicalChunks[i], _chunkSize);
	}
}

void AllocatedMemory::acquire(uintptr_t offset, size_t length) {
	// TODO: Mark the pages as locked.
	_populateRange(offset, length);
}

void AllocatedMemory::release(uintptr_t offset, size_t length) {
	// TODO: Mark the pages as unlocked.
	(void)offset;
	(void)length;
}

PhysicalAddr AllocatedMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	size_t index = offset / _chunkSize;
	assert(index < _physicalChunks.size());
	return _physicalChunks[index];
}

PhysicalAddr AllocatedMemory::fetchRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	_populateRange(offset, kPageSize);

	size_t index = offset / _chunkSize;
	assert(index < _physicalChunks.size());
	assert(_physicalChunks[index] != PhysicalAddr(-1));
	return _physicalChunks[index];
}

size_t AllocatedMemory::getLength() {
	return _physicalChunks.size() * _chunkSize;
}

void AllocatedMemory::_populateRange(uintptr_t offset, size_t length) {
	auto range = alignRange(offset, length, _chunkSize);
	for(uintptr_t progress = 0; progress < range.get<1>(); progress += _chunkSize) {
		size_t index = (range.get<0>() + progress) / _chunkSize;
		assert(index < _physicalChunks.size());
		if(_physicalChunks[index] != PhysicalAddr(-1))
			continue;

		auto physical = physicalAllocator->allocate(_chunkSize);
		assert(physical != PhysicalAddr(-1));
		assert(!(physical % _chunkAlign));

		for(size_t pg_progress = 0; pg_progress < _chunkSize; pg_progress += kPageSize) {
			PageAccessor accessor{generalWindow, physical + pg_progress};
			memset(accessor.get(), 0, kPageSize);
		}
		_physicalChunks[index] = physical;
	}
}

// --------------------------------------------------------
// ManagedSpace
// --------------------------------------------------------

ManagedSpace::ManagedSpace(size_t length)
: physicalPages(*kernelAlloc), loadState(*kernelAlloc) {
	assert(length % kPageSize == 0);
	physicalPages.resize(length / kPageSize, PhysicalAddr(-1));
	loadState.resize(length / kPageSize, kStateMissing);
}

ManagedSpace::~ManagedSpace() {
	assert(!"Implement this");
}

void ManagedSpace::progressLoads() {
	// TODO: this function could issue loads > a single kPageSize
	while(!initiateLoadQueue.empty()) {
		frigg::UnsafePtr<InitiateBase> initiate = initiateLoadQueue.front();

		size_t index = (initiate->offset + initiate->progress) / kPageSize;
		if(loadState[index] == kStateMissing) {
			if(handleLoadQueue.empty())
				break;

			loadState[index] = kStateLoading;

			frigg::SharedPtr<ManageBase> handle = handleLoadQueue.removeFront();
			handle->complete(kErrSuccess, initiate->offset + initiate->progress, kPageSize);

			initiate->progress += kPageSize;
		}else if(loadState[index] == kStateLoading) {
			initiate->progress += kPageSize;
		}else{
			assert(loadState[index] == kStateLoaded);
			initiate->progress += kPageSize;
		}

		if(initiate->progress == initiate->length) {
			if(isComplete(initiate)) {
				initiate->complete(kErrSuccess);
				initiateLoadQueue.removeFront();
			}else{
				pendingLoadQueue.addBack(initiateLoadQueue.removeFront());
			}
		}
	}
}

bool ManagedSpace::isComplete(frigg::UnsafePtr<InitiateBase> initiate) {
	for(size_t p = 0; p < initiate->length; p += kPageSize) {
		size_t index = (initiate->offset + p) / kPageSize;
		if(loadState[index] != kStateLoaded)
			return false;
	}
	return true;
}

// --------------------------------------------------------
// BackingMemory
// --------------------------------------------------------

void BackingMemory::acquire(uintptr_t offset, size_t length) {
	(void)offset;
	(void)length;
	assert(!"Implement this");
}

void BackingMemory::release(uintptr_t offset, size_t length) {
	(void)offset;
	(void)length;
	assert(!"Implement this");
}

PhysicalAddr BackingMemory::peekRange(uintptr_t offset) {
	(void)offset;
	assert(!"Implement this");
	__builtin_unreachable();
}

PhysicalAddr BackingMemory::fetchRange(uintptr_t offset) {
	(void)offset;
	assert(!"Implement this");
	__builtin_unreachable();
}

size_t BackingMemory::getLength() {
	return _managed->physicalPages.size() * kPageSize;
}

// TODO: This has to be integrated into acquire()/fetchRange().
/*
PhysicalAddr BackingMemory::grabPage(GrabIntent, size_t offset) {
	assert(offset % kPageSize == 0);
	
	size_t index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());

	if(_managed->physicalPages[index] == PhysicalAddr(-1)) {
		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		
		PageAccessor accessor{generalWindow, physical};
		memset(accessor.get(), 0, kPageSize);
		_managed->physicalPages[index] = physical;
	}

	return _managed->physicalPages[index];
}
*/

void BackingMemory::submitHandleLoad(frigg::SharedPtr<ManageBase> handle) {
	_managed->handleLoadQueue.addBack(frigg::move(handle));
	_managed->progressLoads();
}

void BackingMemory::completeLoad(size_t offset, size_t length) {
	assert((offset % kPageSize) == 0);
	assert((length % kPageSize) == 0);
	assert((offset + length) / kPageSize <= _managed->physicalPages.size());

/*	assert(length == kPageSize);
	auto inspect = (unsigned char *)physicalToVirtual(_managed->physicalPages[offset / kPageSize]);
	auto log = frigg::infoLogger() << "dump";
	for(size_t b = 0; b < kPageSize; b += 16) {
		log << frigg::logHex(offset + b) << "   ";
		for(size_t i = 0; i < 16; i++)
			log << " " << frigg::logHex(inspect[b + i]);
		log << "\n";
	}
	log << frigg::endLog;*/

	for(size_t p = 0; p < length; p += kPageSize) {
		size_t index = (offset + p) / kPageSize;
		assert(_managed->loadState[index] == ManagedSpace::kStateLoading);
		_managed->loadState[index] = ManagedSpace::kStateLoaded;
	}

	for(auto it = _managed->pendingLoadQueue.frontIter(); it; ) {
		auto it_copy = it++;
		if(_managed->isComplete(*it_copy)) {
			(*it_copy)->complete(kErrSuccess);
			_managed->pendingLoadQueue.remove(it_copy);
		}
	}
}

// --------------------------------------------------------
// FrontalMemory
// --------------------------------------------------------

void FrontalMemory::acquire(uintptr_t offset, size_t length) {
	(void)offset;
	(void)length;
	assert(!"Implement this");
}

void FrontalMemory::release(uintptr_t offset, size_t length) {
	(void)offset;
	(void)length;
	assert(!"Implement this");
}

PhysicalAddr FrontalMemory::peekRange(uintptr_t offset) {
	(void)offset;
	assert(!"Implement this");
	__builtin_unreachable();
}

PhysicalAddr FrontalMemory::fetchRange(uintptr_t offset) {
	(void)offset;
	assert(!"Implement this");
	__builtin_unreachable();
}

size_t FrontalMemory::getLength() {
	return _managed->physicalPages.size() * kPageSize;
}

// TODO: This has to be integrated into acquire()/fetchRange().
/*
PhysicalAddr FrontalMemory::grabPage(GrabIntent grab_intent, size_t offset) {
	assert(offset % kPageSize == 0);

	size_t index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());

	if(grab_intent & kGrabQuery) {
		if(_managed->loadState[index] != ManagedSpace::kStateLoaded)
			return PhysicalAddr(-1);
		return _managed->physicalPages[index];
	}else{
		assert(grab_intent & kGrabFetch);

		if(_managed->loadState[index] != ManagedSpace::kStateLoaded) {
			assert(!(grab_intent & kGrabDontRequireBacking));
			auto this_thread = getCurrentThread();

			std::atomic<bool> complete(false);
			auto functor = [&] (Error error) {
				assert(error == kErrSuccess);
				complete.store(true, std::memory_order_release);
				Thread::unblockOther(this_thread);
			};

			// TODO: Store the initiation object on the stack.
			// This ensures that we can not run out of kernel heap memory here.
			auto initiate = frigg::makeShared<Initiate<decltype(functor)>>(*kernelAlloc,
					offset, kPageSize, frigg::move(functor));
			_managed->initiateLoadQueue.addBack(frigg::move(initiate));
			_managed->progressLoads();

			Thread::blockCurrentWhile([&] {
				return !complete.load(std::memory_order_acquire);
			});
		}

		PhysicalAddr physical = _managed->physicalPages[index];
		assert(physical != PhysicalAddr(-1));
		return physical;
	}
}
*/

void FrontalMemory::submitInitiateLoad(frigg::SharedPtr<InitiateBase> initiate) {
	assert(initiate->offset % kPageSize == 0);
	assert(initiate->length % kPageSize == 0);
	assert((initiate->offset + initiate->length) / kPageSize
			<= _managed->physicalPages.size());
	
	_managed->initiateLoadQueue.addBack(frigg::move(initiate));
	_managed->progressLoads();
}

// --------------------------------------------------------
// SpaceAggregator
// --------------------------------------------------------

bool SpaceAggregator::aggregate(Mapping *mapping) {
	size_t hole = 0;
	if(mapping->type() == MappingType::hole)
		hole = mapping->length();
	if(SpaceTree::get_left(mapping) && SpaceTree::get_left(mapping)->largestHole > hole)
		hole = SpaceTree::get_left(mapping)->largestHole;
	if(SpaceTree::get_right(mapping) && SpaceTree::get_right(mapping)->largestHole > hole)
		hole = SpaceTree::get_right(mapping)->largestHole;
	
	if(mapping->largestHole == hole)
		return false;
	mapping->largestHole = hole;
	return true;
}

bool SpaceAggregator::check_invariant(SpaceTree &tree, Mapping *node) {
	Mapping *pred = tree.predecessor(node);
	Mapping *succ = tree.successor(node);

	// check largest hole invariant.
	size_t hole = 0;
	if(node->type() == MappingType::hole)
		hole = node->length();
	if(tree.get_left(node) && tree.get_left(node)->largestHole > hole)
		hole = tree.get_left(node)->largestHole;
	if(tree.get_right(node) && tree.get_right(node)->largestHole > hole)
		hole = tree.get_right(node)->largestHole;
	
	if(node->largestHole != hole) {
		frigg::infoLogger() << "largestHole violation: " << "Expected " << hole
				<< ", got " << node->largestHole << "." << frigg::endLog;
		return false;
	}

	// check non-overlapping memory areas invariant.
	if(pred && node->address() < pred->address() + pred->length()) {
		frigg::infoLogger() << "Non-overlapping (left) violation" << frigg::endLog;
		return false;
	}
	if(succ && node->address() + node->length() > succ->address()) {
		frigg::infoLogger() << "Non-overlapping (right) violation" << frigg::endLog;
		return false;
	}
	
	return true;
}

// --------------------------------------------------------
// Mapping
// --------------------------------------------------------

Mapping::Mapping(AddressSpace *owner, VirtualAddr base_address, size_t length,
		MappingFlags flags)
: _owner{owner}, _address{base_address}, _length{length},
		_flags{flags}, largestHole{0} { }

// --------------------------------------------------------
// HoleMapping
// --------------------------------------------------------

HoleMapping::HoleMapping(AddressSpace *owner, VirtualAddr address, size_t length,
		MappingFlags flags)
: Mapping{owner, address, length, flags} {
	// TODO: Is this even necessary?
	largestHole = length;
}

Mapping *HoleMapping::shareMapping(AddressSpace *dest_space) {
	(void)dest_space;
	assert(!"Cannot share a HoleMapping");
	__builtin_unreachable();
}

Mapping *HoleMapping::copyMapping(AddressSpace *dest_space) {
	(void)dest_space;
	assert(!"Cannot share a HoleMapping");
	__builtin_unreachable();
}

Mapping *HoleMapping::copyOnWrite(AddressSpace *dest_space) {
	(void)dest_space;
	assert(!"Cannot copy-on-write a HoleMapping");
	__builtin_unreachable();
}

void HoleMapping::install(bool overwrite) {
	// We do not need to do anything here.
	// TODO: Ensure that this is never called?
	(void)overwrite;
}	

void HoleMapping::uninstall(bool clear) {
	// See comments of HoleMapping.
	(void)clear;
}

PhysicalAddr HoleMapping::grabPhysical(VirtualAddr disp) {
	(void)disp;
	assert(!"Cannot grab pages of HoleMapping");
	__builtin_unreachable();
}

bool HoleMapping::handleFault(VirtualAddr disp, uint32_t fault_flags) {
	(void)disp;
	(void)fault_flags;
	assert(!"HoleMappings should never fault");
	__builtin_unreachable();
}

// --------------------------------------------------------
// NormalMapping
// --------------------------------------------------------

NormalMapping::NormalMapping(AddressSpace *owner, VirtualAddr address, size_t length,
		MappingFlags flags, frigg::SharedPtr<Memory> memory, uintptr_t offset)
: Mapping{owner, address, length, flags}, _memory{frigg::move(memory)}, _offset{offset} { }

Mapping *NormalMapping::shareMapping(AddressSpace *dest_space) {
	// TODO: Always keep the exact flags?
	return frigg::construct<NormalMapping>(*kernelAlloc, dest_space,
			address(), length(), flags(), _memory, _offset);
}

Mapping *NormalMapping::copyMapping(AddressSpace *dest_space) {
	// TODO: Always keep the exact flags?
	// TODO: We do not need to copy the whole memory object.
	// TODO: Call a copy operation of the corresponding memory object here.
	auto dest_memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc,
			_memory->getLength(), kPageSize, kPageSize);
	Memory::transfer(dest_memory, 0, _memory, 0, _memory->getLength());
	
	return frigg::construct<NormalMapping>(*kernelAlloc, dest_space,
			address(), length(), flags(), frigg::move(dest_memory), _offset);
}

Mapping *NormalMapping::copyOnWrite(AddressSpace *dest_space) {
	auto chain = frigg::makeShared<CowChain>(*kernelAlloc);
	chain->memory = _memory;
	chain->offset = _offset;
	chain->mask.resize(length() >> kPageShift, true);
	return frigg::construct<CowMapping>(*kernelAlloc, dest_space,
			address(), length(), flags(), frigg::move(chain));
}

void NormalMapping::install(bool overwrite) {
	uint32_t page_flags = 0;
	if((flags() & MappingFlags::permissionMask) == MappingFlags::readWrite) {
		page_flags |= page_access::write;
	}else if((flags() & MappingFlags::permissionMask) == MappingFlags::readExecute) {
		page_flags |= page_access::execute;
	}else{
		assert((flags() & MappingFlags::permissionMask) == MappingFlags::readOnly);
	}

	for(size_t progress = 0; progress < length(); progress += kPageSize) {
		// TODO: Add a don't-require-backing flag to peekRange.
		//if(flags() & MappingFlags::dontRequireBacking)
		//	grab_flags |= kGrabDontRequireBacking;

		PhysicalAddr physical = _memory->peekRange(_offset + progress);

		VirtualAddr vaddr = address() + progress;
		if(overwrite && owner()->_pageSpace.isMapped(vaddr)) {
			owner()->_pageSpace.unmapSingle4k(vaddr);
		}else{
			assert(!owner()->_pageSpace.isMapped(vaddr));
		}
		if(physical != PhysicalAddr(-1))
			owner()->_pageSpace.mapSingle4k(vaddr, physical, true, page_flags);
	}
}

void NormalMapping::uninstall(bool clear) {
	if(!clear)
		return;

	for(size_t progress = 0; progress < length(); progress += kPageSize) {
		VirtualAddr vaddr = address() + progress;
		if(owner()->_pageSpace.isMapped(vaddr))
			owner()->_pageSpace.unmapSingle4k(vaddr);
	}
}

PhysicalAddr NormalMapping::grabPhysical(VirtualAddr disp) {
	// TODO: Allocate missing pages for OnDemand or CopyOnWrite pages.

	// TODO: Add a don't-require-backing flag to peekRange.
	//if(flags() & MappingFlags::dontRequireBacking)
	//	grab_flags |= kGrabDontRequireBacking;

	return _memory->fetchRange(_offset + disp);
}

bool NormalMapping::handleFault(VirtualAddr disp, uint32_t fault_flags) {
	(void)fault_flags;
	
	uint32_t page_flags = 0;
	if((flags() & MappingFlags::permissionMask) == MappingFlags::readWrite) {
		page_flags |= page_access::write;
	}else if((flags() & MappingFlags::permissionMask) == MappingFlags::readExecute) {
		page_flags |= page_access::execute;
	}else{
		assert((flags() & MappingFlags::permissionMask) == MappingFlags::readOnly);
	}

	auto page = disp & ~(kPageSize - 1);
	auto physical = _memory->fetchRange(page);
	auto vaddr = address() + page;
	// TODO: This can actually happen!
	assert(!owner()->_pageSpace.isMapped(vaddr));
	owner()->_pageSpace.mapSingle4k(vaddr, physical, true, page_flags);
	return true;
}

// --------------------------------------------------------
// CowMapping
// --------------------------------------------------------

CowMapping::CowMapping(AddressSpace *owner, VirtualAddr address, size_t length,
		MappingFlags flags, frigg::SharedPtr<CowChain> chain)
: Mapping{owner, address, length, flags}, _mask{*kernelAlloc}, _chain{frigg::move(chain)} {
	_copy = frigg::makeShared<AllocatedMemory>(*kernelAlloc, length, kPageSize, kPageSize);
	_mask.resize(length >> kPageShift, false);
}

Mapping *CowMapping::shareMapping(AddressSpace *dest_space) {
	(void)dest_space;
	assert(!"Fix this");
	__builtin_unreachable();
}

Mapping *CowMapping::copyMapping(AddressSpace *dest_space) {
	(void)dest_space;
	assert(!"Fix this");
	__builtin_unreachable();
}

Mapping *CowMapping::copyOnWrite(AddressSpace *dest_space) {
	auto sub_chain = frigg::makeShared<CowChain>(*kernelAlloc);
	sub_chain->memory = _copy;
	sub_chain->offset = 0;
	sub_chain->super = _chain;
	sub_chain->mask.resize(length() >> kPageShift, false);
	for(size_t i = 0; i < (length() >> kPageShift); ++i)
		sub_chain->mask[i] = _mask[i]; // TODO: Use a copy constructor.
	return frigg::construct<CowMapping>(*kernelAlloc, dest_space,
			address(), length(), flags(), frigg::move(sub_chain));
}

void CowMapping::install(bool overwrite) {
	assert((flags() & MappingFlags::permissionMask) == MappingFlags::readWrite);
	
	// For now we just unmap everything. TODO: Map available pages.
	for(size_t progress = 0; progress < length(); progress += kPageSize) {
		VirtualAddr vaddr = address() + progress;
		if(overwrite && owner()->_pageSpace.isMapped(vaddr)) {
			owner()->_pageSpace.unmapSingle4k(vaddr);
		}else{
			assert(!owner()->_pageSpace.isMapped(vaddr));
		}
	}
}

void CowMapping::uninstall(bool clear) {
	if(!clear)
		return;

	for(size_t progress = 0; progress < length(); progress += kPageSize) {
		VirtualAddr vaddr = address() + progress;
		if(owner()->_pageSpace.isMapped(vaddr))
			owner()->_pageSpace.unmapSingle4k(vaddr);
	}
}

PhysicalAddr CowMapping::grabPhysical(VirtualAddr disp) {
	return _retrievePage(disp);
}

bool CowMapping::handleFault(VirtualAddr disp, uint32_t fault_flags) {
	(void)fault_flags; // TODO: Assert that it is a write-fault.
	auto page = disp & ~(kPageSize - 1);
	// TODO: This may actually happen if there are multiple threads
	// racing for the page.
	assert(!_mask[page >> kPageShift]);
	_retrievePage(disp);
	return true;
}

PhysicalAddr CowMapping::_retrievePage(VirtualAddr disp) {
	auto page = disp & ~(kPageSize - 1);
	if(_mask[page >> kPageShift]) {
		auto physical = _copy->fetchRange(page);
		assert(physical != PhysicalAddr(-1));
		return physical;
	}

	for(frigg::UnsafePtr<CowChain> ch = _chain; ch; ch = ch->super) {
		if(!ch->mask[page >> kPageShift])
			continue;

		Memory::transfer(_copy, page, ch->memory, ch->offset + page, kPageSize);
		_mask[page >> kPageShift] = true;

		auto physical = _copy->fetchRange(page);

		// We have to map the page immediately after copying it.
		// This ensures that no racing threads still see the original page.
		owner()->_pageSpace.mapSingle4k(address() + page, physical,
				true, page_access::write);
		return physical;
	}

	assert(!"Fix this");
	__builtin_unreachable();
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

AddressSpace::AddressSpace() { }

AddressSpace::~AddressSpace() {
	assert(!"Fix this");
	// TODO: fix this by iteratively freeing all nodes of *spaceTree*.
}

void AddressSpace::setupDefaultMappings() {
	auto mapping = frigg::construct<HoleMapping>(*kernelAlloc, this,
			0x100000, 0x7ffffff00000, MappingFlags::null);
	spaceTree.insert(mapping);
}

void AddressSpace::map(Guard &guard,
		KernelUnsafePtr<Memory> memory, VirtualAddr address,
		size_t offset, size_t length, uint32_t flags, VirtualAddr *actual_address) {
	assert(guard.protects(&lock));
	assert(length);
	assert(!(length % kPageSize));

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
	
	if(flags & kMapDropAtFork) {
		mapping_flags |= MappingFlags::dropAtFork;
	}else if(flags & kMapShareAtFork) {
		mapping_flags |= MappingFlags::shareAtFork;
	}else if(flags & kMapCopyOnWriteAtFork) {
		mapping_flags |= MappingFlags::copyOnWriteAtFork;
	}
	
	constexpr uint32_t mask = kMapReadOnly | kMapReadExecute | kMapReadWrite;
	if((flags & mask) == kMapReadWrite) {
		mapping_flags |= MappingFlags::readWrite;
	}else if((flags & mask) == kMapReadExecute) {
		mapping_flags |= MappingFlags::readExecute;
	}else{
		assert((flags & mask) == kMapReadOnly);
		mapping_flags |= MappingFlags::readOnly;
	}
	
	if(flags & kMapDontRequireBacking)
		mapping_flags |= MappingFlags::dontRequireBacking;

	auto mapping = frigg::construct<NormalMapping>(*kernelAlloc, this, target, length,
			static_cast<MappingFlags>(mapping_flags), memory.toShared(), offset);
	
	// Install the new mapping object.
	spaceTree.insert(mapping);
	assert(!(flags & kMapPopulate));
	mapping->install(false);

	*actual_address = target;
}

void AddressSpace::unmap(Guard &guard, VirtualAddr address, size_t length) {
	assert(guard.protects(&lock));

	Mapping *mapping = getMapping(address);
	assert(mapping);

	// TODO: allow shrink of mapping
	assert(mapping->address() == address);
	assert(mapping->length() == length);
	mapping->uninstall(true);

	auto predecessor = SpaceTree::predecessor(mapping);
	auto successor = SpaceTree::successor(mapping);
	assert(predecessor->address() + predecessor->length() == mapping->address());
	assert(mapping->address() + mapping->length() == successor->address());
	
	if(predecessor && predecessor->type() == MappingType::hole
			&& successor && successor->type() == MappingType::hole) {
		auto hole = frigg::construct<HoleMapping>(*kernelAlloc, this, predecessor->address(),
				predecessor->length() + mapping->length() + successor->length(),
				MappingFlags::null);

		spaceTree.remove(predecessor);
		spaceTree.remove(mapping);
		spaceTree.remove(successor);
		frigg::destruct(*kernelAlloc, predecessor);
		frigg::destruct(*kernelAlloc, mapping);
		frigg::destruct(*kernelAlloc, successor);
		spaceTree.insert(hole);
	}else if(predecessor && predecessor->type() == MappingType::hole) {
		auto hole = frigg::construct<HoleMapping>(*kernelAlloc, this,
				predecessor->address(), predecessor->length() + mapping->length(),
				MappingFlags::null);

		spaceTree.remove(predecessor);
		spaceTree.remove(mapping);
		frigg::destruct(*kernelAlloc, predecessor);
		frigg::destruct(*kernelAlloc, mapping);
		spaceTree.insert(hole);
	}else if(successor && successor->type() == MappingType::hole) {
		auto hole = frigg::construct<HoleMapping>(*kernelAlloc, this,
				mapping->address(), mapping->length() + successor->length(),
				MappingFlags::null);

		spaceTree.remove(mapping);
		spaceTree.remove(successor);
		frigg::destruct(*kernelAlloc, mapping);
		frigg::destruct(*kernelAlloc, successor);
		spaceTree.insert(hole);
	}else{
		auto hole = frigg::construct<HoleMapping>(*kernelAlloc, this,
				mapping->address(), mapping->length(), MappingFlags::null);

		spaceTree.remove(mapping);
		frigg::destruct(*kernelAlloc, mapping);
		spaceTree.insert(hole);
	}
}

bool AddressSpace::handleFault(Guard &guard, VirtualAddr address, uint32_t fault_flags) {
	assert(guard.protects(&lock));

	// TODO: It seems that this is not invoked for on-demand allocation
	// of AllocatedMemory objects!

	Mapping *mapping = getMapping(address);
	if(!mapping || mapping->type() == MappingType::hole)
		return false;
	return mapping->handleFault(address - mapping->address(), fault_flags);
}

KernelSharedPtr<AddressSpace> AddressSpace::fork(Guard &guard) {
	assert(guard.protects(&lock));

	auto forked = frigg::makeShared<AddressSpace>(*kernelAlloc);
	cloneRecursive(spaceTree.first(), forked.get());

	return frigg::move(forked);
}

PhysicalAddr AddressSpace::grabPhysical(Guard &guard, VirtualAddr address) {
	assert(guard.protects(&lock));
	assert((address % kPageSize) == 0);

	Mapping *mapping = getMapping(address);
	assert(mapping);
	return mapping->grabPhysical(address - mapping->address());
}

void AddressSpace::activate() {
	_pageSpace.activate();
}

Mapping *AddressSpace::getMapping(VirtualAddr address) {
	Mapping *current = spaceTree.get_root();
	
	while(current != nullptr) {
		if(address < current->address()) {
			current = SpaceTree::get_left(current);
		}else if(address >= current->address() + current->length()) {
			current = SpaceTree::get_right(current);
		}else{
			assert(address >= current->address()
					&& address < current->address() + current->length());
			return current;
		}
	}

	return nullptr;
}

VirtualAddr AddressSpace::_allocate(size_t length, MapFlags flags) {
	assert(length > 0);
	assert((length % kPageSize) == 0);
//	frigg::infoLogger() << "Allocate virtual memory area"
//			<< ", size: 0x" << frigg::logHex(length) << frigg::endLog;

	if(spaceTree.get_root()->largestHole < length)
		return 0; // TODO: Return something else here?
	
	return _allocateDfs(spaceTree.get_root(), length, flags);
}

VirtualAddr AddressSpace::_allocateDfs(Mapping *mapping, size_t length,
		MapFlags flags) {
	if((flags & kMapPreferBottom) != 0) {
		// Try to allocate memory at the bottom of the range.
		if(mapping->type() == MappingType::hole && mapping->length() >= length) {
			splitHole(mapping, 0, length);
			return mapping->address();
		}
		
		if(SpaceTree::get_left(mapping) && SpaceTree::get_left(mapping)->largestHole >= length)
			return _allocateDfs(SpaceTree::get_left(mapping), length, flags);
		
		assert(SpaceTree::get_right(mapping));
		assert(SpaceTree::get_right(mapping)->largestHole >= length);
		return _allocateDfs(SpaceTree::get_right(mapping), length, flags);
	}else{
		// Try to allocate memory at the top of the range.
		assert((flags & kMapPreferTop) != 0);
		if(mapping->type() == MappingType::hole && mapping->length() >= length) {
			size_t offset = mapping->length() - length;
			splitHole(mapping, offset, length);
			return mapping->address() + offset;
		}

		if(SpaceTree::get_right(mapping) && SpaceTree::get_right(mapping)->largestHole >= length)
			return _allocateDfs(SpaceTree::get_right(mapping), length, flags);
		
		assert(SpaceTree::get_left(mapping));
		assert(SpaceTree::get_left(mapping)->largestHole >= length);
		return _allocateDfs(SpaceTree::get_left(mapping), length, flags);
	}
}

VirtualAddr AddressSpace::_allocateAt(VirtualAddr address, size_t length) {
	assert(!(address % kPageSize));
	assert(!(length % kPageSize));

	Mapping *hole = getMapping(address);
	assert(hole);
	assert(hole->type() == MappingType::hole);
	
	splitHole(hole, address - hole->address(), length);
	return address;
}

void AddressSpace::cloneRecursive(Mapping *mapping, AddressSpace *dest_space) {
	auto successor = SpaceTree::successor(mapping);

	if(mapping->type() == MappingType::hole) {
		auto dest_mapping = frigg::construct<HoleMapping>(*kernelAlloc, this,
				mapping->address(), mapping->length(), MappingFlags::null);
		dest_space->spaceTree.insert(dest_mapping);
	}else if(mapping->flags() & MappingFlags::dropAtFork) {
		// TODO: Merge this hole into adjacent holes.
		auto dest_mapping = frigg::construct<HoleMapping>(*kernelAlloc, this,
				mapping->address(), mapping->length(), MappingFlags::null);
		dest_space->spaceTree.insert(dest_mapping);
	}else if(mapping->flags() & MappingFlags::shareAtFork) {
		auto dest_mapping = mapping->shareMapping(dest_space);

		dest_space->spaceTree.insert(dest_mapping);
		dest_mapping->install(false);
	}else if(mapping->flags() & MappingFlags::copyOnWriteAtFork) {
		// TODO: Copy-on-write if possible and plain copy otherwise.
		// TODO: Decide if we want a copy-on-write or a real copy of the mapping.
		// * Pinned mappings prevent CoW.
		//     This is necessary because CoW may change mapped pages
		//     in the original space.
		// * Futexes attached to the memory object prevent CoW.
		//     This ensures that processes do not miss wake ups in the original space.
		auto new_mapping = mapping->copyOnWrite(this);
		auto dest_mapping = mapping->copyOnWrite(dest_space);

		spaceTree.remove(mapping);
		spaceTree.insert(new_mapping);
		dest_space->spaceTree.insert(dest_mapping);
		mapping->uninstall(false);
		new_mapping->install(true);
		dest_mapping->install(false);
		frigg::destruct(*kernelAlloc, mapping);
	}else{
		assert(!"Illegal mapping type");
	}

	if(successor)
		cloneRecursive(successor, dest_space);
}

void AddressSpace::splitHole(Mapping *hole, VirtualAddr offset, size_t length) {
	assert(hole->type() == MappingType::hole);
	assert(length);
	assert(offset + length <= hole->length());
	
	spaceTree.remove(hole);

	if(offset) {
		auto predecessor = frigg::construct<HoleMapping>(*kernelAlloc, this,
				hole->address(), offset, MappingFlags::null);
		spaceTree.insert(predecessor);
	}

	if(offset + length < hole->length()) {
		auto successor = frigg::construct<HoleMapping>(*kernelAlloc, this,
				hole->address() + (offset + length), hole->length() - (offset + length),
				MappingFlags::null);
		spaceTree.insert(successor);
	}
	
	frigg::destruct(*kernelAlloc, hole);
}

} // namespace thor

