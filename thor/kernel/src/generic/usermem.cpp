
#include "kernel.hpp"

namespace thor {
	
// --------------------------------------------------------
// Memory
// --------------------------------------------------------

void Memory::transfer(frigg::UnsafePtr<Memory> dest_memory, uintptr_t dest_offset,
		frigg::UnsafePtr<Memory> src_memory, uintptr_t src_offset, size_t length) {
	PhysicalChunkAllocator::Guard lock(&physicalAllocator->lock,
			frigg::dontLock);
	
	size_t progress = 0;
	while(progress < length) {
		auto dest_misalign = (dest_offset + progress) % kPageSize;
		auto src_misalign = (src_offset + progress) % kPageSize;
		size_t chunk = frigg::min(frigg::min(kPageSize - dest_misalign,
				kPageSize - src_misalign), length - progress);
		
		PhysicalAddr dest_page = dest_memory->grabPage(lock,
				kGrabFetch | kGrabWrite, dest_offset + progress - dest_misalign);
		PhysicalAddr src_page = src_memory->grabPage(lock,
				kGrabFetch | kGrabRead, src_offset + progress - dest_misalign);
		assert(dest_page != PhysicalAddr(-1));
		assert(src_page != PhysicalAddr(-1));
		
		PageAccessor dest_accessor{generalWindow, dest_page};
		PageAccessor src_accessor{generalWindow, src_page};
		memcpy((uint8_t *)dest_accessor.get() + dest_misalign,
				(uint8_t *)src_accessor.get() + src_misalign, chunk);
		
		progress += chunk;
	}
}

size_t Memory::getLength() {
	switch(tag()) {
	case MemoryTag::hardware: return static_cast<HardwareMemory *>(this)->getLength();
	case MemoryTag::allocated: return static_cast<AllocatedMemory *>(this)->getLength();
	case MemoryTag::backing: return static_cast<BackingMemory *>(this)->getLength();
	case MemoryTag::frontal: return static_cast<FrontalMemory *>(this)->getLength();
	case MemoryTag::copyOnWrite: return static_cast<CopyOnWriteMemory *>(this)->getLength();
	default:
		frigg::panicLogger() << "Memory::getLength(): Unexpected tag" << frigg::endLog;
		__builtin_unreachable();
	}
}

PhysicalAddr Memory::grabPage(PhysicalChunkAllocator::Guard &physical_guard,
		GrabIntent grab_flags, size_t offset) {
	assert((grab_flags & kGrabQuery) || (grab_flags & kGrabFetch));
	assert(!((grab_flags & kGrabQuery) && (grab_flags & kGrabFetch)));
	switch(tag()) {
	case MemoryTag::hardware:
		return static_cast<HardwareMemory *>(this)->grabPage(physical_guard,
				grab_flags, offset);
	case MemoryTag::allocated:
		return static_cast<AllocatedMemory *>(this)->grabPage(physical_guard,
				grab_flags, offset);
	case MemoryTag::backing:
		return static_cast<BackingMemory *>(this)->grabPage(physical_guard,
				grab_flags, offset);
	case MemoryTag::frontal:
		return static_cast<FrontalMemory *>(this)->grabPage(physical_guard,
				grab_flags, offset);
	case MemoryTag::copyOnWrite:
		return static_cast<CopyOnWriteMemory *>(this)->grabPage(physical_guard,
				grab_flags, offset);
	default:
		frigg::panicLogger() << "Memory::grabPage(): Unexpected tag" << frigg::endLog;
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
	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock,
			frigg::dontLock);
	
	size_t progress = 0;

	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, length);
		PhysicalAddr page = grabPage(physical_guard, kGrabFetch | kGrabRead, offset - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{generalWindow, page};
		memcpy(buffer, (uint8_t *)accessor.get() + misalign, prefix);
		progress += prefix;
	}

	while(length - progress >= kPageSize) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = grabPage(physical_guard, kGrabFetch | kGrabRead, offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{generalWindow, page};
		memcpy((uint8_t *)buffer + progress, accessor.get(), kPageSize);
		progress += kPageSize;
	}

	if(length - progress > 0) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = grabPage(physical_guard, kGrabFetch | kGrabRead, offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{generalWindow, page};
		memcpy((uint8_t *)buffer + progress, accessor.get(), length - progress);
	}
}

void Memory::copyFrom(size_t offset, void *buffer, size_t length) {
	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock,
			frigg::dontLock);
	
	size_t progress = 0;

	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, length);
		PhysicalAddr page = grabPage(physical_guard, kGrabFetch | kGrabWrite, offset - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{generalWindow, page};
		memcpy((uint8_t *)accessor.get() + misalign, buffer, prefix);
		progress += prefix;
	}

	while(length - progress >= kPageSize) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = grabPage(physical_guard, kGrabFetch | kGrabWrite, offset + progress);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{generalWindow, page};
		memcpy(accessor.get(), (uint8_t *)buffer + progress, kPageSize);
		progress += kPageSize;
	}

	if(length - progress > 0) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = grabPage(physical_guard, kGrabFetch | kGrabWrite, offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{generalWindow, page};
		memcpy(accessor.get(), (uint8_t *)buffer + progress, length - progress);
	}
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

size_t HardwareMemory::getLength() {
	return _length;
}

PhysicalAddr HardwareMemory::grabPage(PhysicalChunkAllocator::Guard &,
		GrabIntent, size_t offset) {
	assert(offset % kPageSize == 0);
	assert(offset + kPageSize <= _length);
	return _base + offset;
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
	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
	for(size_t i = 0; i < _physicalChunks.size(); ++i) {
		if(_physicalChunks[i] != PhysicalAddr(-1))
			physicalAllocator->free(physical_guard, _physicalChunks[i], _chunkSize);
	}
}

size_t AllocatedMemory::getLength() {
	return _physicalChunks.size() * _chunkSize;
}

PhysicalAddr AllocatedMemory::grabPage(PhysicalChunkAllocator::Guard &physical_guard,
		GrabIntent, size_t offset) {
	assert(offset % kPageSize == 0);
	
	size_t index = offset / _chunkSize;
	size_t disp = offset % _chunkSize;
	assert(index < _physicalChunks.size());

	if(_physicalChunks[index] == PhysicalAddr(-1)) {
		if(!physical_guard.isLocked())
			physical_guard.lock();
		PhysicalAddr physical = physicalAllocator->allocate(physical_guard, _chunkSize);
		assert(physical % _chunkAlign == 0);
		
		for(size_t progress = 0; progress < _chunkSize; progress += kPageSize) {
			PageAccessor accessor{generalWindow, physical + progress};
			memset(accessor.get(), 0, kPageSize);
		}
		_physicalChunks[index] = physical;
	}

	return _physicalChunks[index] + disp;
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

size_t BackingMemory::getLength() {
	return _managed->physicalPages.size() * kPageSize;
}

PhysicalAddr BackingMemory::grabPage(PhysicalChunkAllocator::Guard &physical_guard,
		GrabIntent, size_t offset) {
	assert(offset % kPageSize == 0);
	
	size_t index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());

	if(_managed->physicalPages[index] == PhysicalAddr(-1)) {
		if(!physical_guard.isLocked())
			physical_guard.lock();
		PhysicalAddr physical = physicalAllocator->allocate(physical_guard, kPageSize);
		
		PageAccessor accessor{generalWindow, physical};
		memset(accessor.get(), 0, kPageSize);
		_managed->physicalPages[index] = physical;
	}

	return _managed->physicalPages[index];
}

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

size_t FrontalMemory::getLength() {
	return _managed->physicalPages.size() * kPageSize;
}

PhysicalAddr FrontalMemory::grabPage(PhysicalChunkAllocator::Guard &,
		GrabIntent grab_intent, size_t offset) {
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

void FrontalMemory::submitInitiateLoad(frigg::SharedPtr<InitiateBase> initiate) {
	assert(initiate->offset % kPageSize == 0);
	assert(initiate->length % kPageSize == 0);
	assert((initiate->offset + initiate->length) / kPageSize
			<= _managed->physicalPages.size());
	
	_managed->initiateLoadQueue.addBack(frigg::move(initiate));
	_managed->progressLoads();
}

// --------------------------------------------------------
// CopyOnWriteMemory
// --------------------------------------------------------

CopyOnWriteMemory::CopyOnWriteMemory(frigg::SharedPtr<Memory> origin)
: Memory(MemoryTag::copyOnWrite), _origin(frigg::move(origin)), _physicalPages(*kernelAlloc) {
	assert(_origin->getLength() % kPageSize == 0);
	_physicalPages.resize(_origin->getLength() / kPageSize, PhysicalAddr(-1));
}

CopyOnWriteMemory::~CopyOnWriteMemory() {
	assert(!"Implement this");
}

size_t CopyOnWriteMemory::getLength() {
	return _physicalPages.size() * kPageSize;
}

PhysicalAddr CopyOnWriteMemory::grabPage(PhysicalChunkAllocator::Guard &physical_guard,
		GrabIntent, size_t offset) {
	assert(offset % kPageSize == 0);
	
	size_t index = offset / kPageSize;
	assert(index < _physicalPages.size());

	// TODO: only copy on write grabs

	if(_physicalPages[index] == PhysicalAddr(-1)) {
		PhysicalAddr origin_physical = _origin->grabPage(physical_guard,
				kGrabFetch | kGrabRead, offset);
		assert(origin_physical != PhysicalAddr(-1));

		if(!physical_guard.isLocked())
			physical_guard.lock();
		PhysicalAddr own_physical = physicalAllocator->allocate(physical_guard, kPageSize);
		
		PageAccessor own_accessor{generalWindow, own_physical};
		PageAccessor origin_accessor{generalWindow, origin_physical};
		memcpy(own_accessor.get(), origin_accessor.get(), kPageSize);
		_physicalPages[index] = own_physical;
	}

	return _physicalPages[index];
}

// --------------------------------------------------------
// SpaceAggregator
// --------------------------------------------------------

bool SpaceAggregator::aggregate(Mapping *mapping) {
	size_t hole = 0;
	if(mapping->type == Mapping::kTypeHole)
		hole = mapping->length;
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
	if(node->type == Mapping::kTypeHole)
		hole = node->length;
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
	if(pred && node->baseAddress < pred->baseAddress + pred->length) {
		frigg::infoLogger() << "Non-overlapping (left) violation" << frigg::endLog;
		return false;
	}
	if(succ && node->baseAddress + node->length > succ->baseAddress) {
		frigg::infoLogger() << "Non-overlapping (right) violation" << frigg::endLog;
		return false;
	}
	
	return true;
}

// --------------------------------------------------------
// Mapping
// --------------------------------------------------------

Mapping::Mapping(Type type, VirtualAddr base_address, size_t length)
: baseAddress(base_address), length(length), type(type),
		largestHole(0), memoryOffset(0), flags(0),
		writePermission(false), executePermission(false) {
	if(type == kTypeHole)
		largestHole = length;
}

Mapping::~Mapping() { }

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

AddressSpace::AddressSpace(PageSpace page_space)
: p_pageSpace(page_space) { }

AddressSpace::~AddressSpace() {
	assert(!"Fix this");
	// TODO: fix this by iteratively freeing all nodes of *spaceTree*.
}

void AddressSpace::setupDefaultMappings() {
	auto mapping = frigg::construct<Mapping>(*kernelAlloc, Mapping::kTypeHole,
			0x100000, 0x7ffffff00000);
	spaceTree.insert(mapping);
}

void AddressSpace::map(Guard &guard,
		KernelUnsafePtr<Memory> memory, VirtualAddr address,
		size_t offset, size_t length, uint32_t flags, VirtualAddr *actual_address) {
	assert(guard.protects(&lock));
	assert(length != 0);
	assert((length % kPageSize) == 0);

	Mapping *mapping;
	if((flags & kMapFixed) != 0) {
		assert(address != 0);
		assert((address % kPageSize) == 0);
		mapping = allocateAt(address, length);
	}else{
		mapping = allocate(length, flags);
	}
	assert(mapping != nullptr);

	mapping->type = Mapping::kTypeMemory;
	mapping->memoryRegion = memory.toShared();
	mapping->memoryOffset = offset;

	uint32_t page_flags = 0;

	constexpr uint32_t mask = kMapReadOnly | kMapReadExecute | kMapReadWrite;
	if((flags & mask) == kMapReadWrite) {
		page_flags |= PageSpace::kAccessWrite;
		mapping->writePermission = true;
	}else if((flags & mask) == kMapReadExecute) {
		page_flags |= PageSpace::kAccessExecute;
		mapping->executePermission = true;
	}else{
		assert((flags & mask) == kMapReadOnly);
	}

	if(flags & kMapDropAtFork) {
		mapping->flags |= Mapping::kFlagDropAtFork;
	}else if(flags & kMapShareAtFork) {
		mapping->flags |= Mapping::kFlagShareAtFork;
	}else if(flags & kMapCopyOnWriteAtFork) {
		mapping->flags |= Mapping::kFlagCopyOnWriteAtFork;
	}
	
	if(flags & kMapDontRequireBacking)
		mapping->flags |= Mapping::kFlagDontRequireBacking;

	{
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock, frigg::dontLock);
		for(size_t page = 0; page < length; page += kPageSize) {
			VirtualAddr vaddr = mapping->baseAddress + page;
			assert(!p_pageSpace.isMapped(vaddr));

			GrabIntent grab_flags = ((flags & kMapPopulate) ? kGrabFetch : kGrabQuery) | kGrabWrite;
			if(mapping->flags & Mapping::kFlagDontRequireBacking)
				grab_flags |= kGrabDontRequireBacking;

			PhysicalAddr physical = memory->grabPage(physical_guard, grab_flags, offset + page);
			if(physical != PhysicalAddr(-1))
				p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
		}
	}

	*actual_address = mapping->baseAddress;
}

void AddressSpace::unmap(Guard &guard, VirtualAddr address, size_t length) {
	assert(guard.protects(&lock));

	Mapping *mapping = getMapping(address);
	assert(mapping != nullptr);
	assert(mapping->type == Mapping::kTypeMemory);

	// TODO: allow shrink of mapping
	assert(mapping->baseAddress == address);
	assert(mapping->length == length);
	
	for(size_t i = 0; i < mapping->length / kPageSize; i++) {
		VirtualAddr vaddr = mapping->baseAddress + i * kPageSize;
		if(p_pageSpace.isMapped(vaddr))
			p_pageSpace.unmapSingle4k(vaddr);
	}

	mapping->memoryRegion = frigg::SharedPtr<Memory>();

	Mapping *lower_ptr = SpaceTree::predecessor(mapping);
	Mapping *higher_ptr = SpaceTree::successor(mapping);
	
	if(lower_ptr && higher_ptr
			&& lower_ptr->type == Mapping::kTypeHole
			&& higher_ptr->type == Mapping::kTypeHole) {
		// grow the lower region and remove both the mapping and the higher region
		size_t mapping_length = mapping->length;
		size_t higher_length = higher_ptr->length;

		spaceTree.remove(mapping);
		spaceTree.remove(higher_ptr);
		frigg::destruct(*kernelAlloc, mapping);
		frigg::destruct(*kernelAlloc, higher_ptr);

		lower_ptr->length += mapping_length + higher_length;
		spaceTree.aggregate_path(lower_ptr);
	}else if(lower_ptr && lower_ptr->type == Mapping::kTypeHole) {
		// grow the lower region and remove the mapping
		size_t mapping_length = mapping->length;

		spaceTree.remove(mapping);
		frigg::destruct(*kernelAlloc, mapping);
		
		lower_ptr->length += mapping_length;
		spaceTree.aggregate_path(lower_ptr);
	}else if(higher_ptr && higher_ptr->type == Mapping::kTypeHole) {
		// grow the higher region and remove the mapping
		size_t mapping_length = mapping->length;

		spaceTree.remove(mapping);
		frigg::destruct(*kernelAlloc, mapping);
		
		higher_ptr->baseAddress -= mapping_length;
		higher_ptr->length += mapping_length;
		spaceTree.aggregate_path(higher_ptr);
	}else{
		// turn the mapping into a hole
		mapping->type = Mapping::kTypeHole;
		spaceTree.aggregate_path(mapping);
	}
}

bool AddressSpace::handleFault(Guard &guard, VirtualAddr address, uint32_t flags) {
	(void)flags;
	assert(guard.protects(&lock));

	Mapping *mapping = getMapping(address);
	if(!mapping)
		return false;
	if(mapping->type != Mapping::kTypeMemory)
		return false;
	
	VirtualAddr page_vaddr = address - (address % kPageSize);
	VirtualAddr page_offset = page_vaddr - mapping->baseAddress;

	uint32_t page_flags = 0;
	if(mapping->writePermission)
		page_flags |= PageSpace::kAccessWrite;
	if(mapping->executePermission)
		page_flags |= PageSpace::kAccessExecute;
	
	KernelUnsafePtr<Memory> memory = mapping->memoryRegion;

	GrabIntent grab_flags = kGrabFetch | kGrabWrite;
	if(mapping->flags & Mapping::kFlagDontRequireBacking)
		grab_flags |= kGrabDontRequireBacking;

	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock, frigg::dontLock);
	PhysicalAddr physical = memory->grabPage(physical_guard, grab_flags,
			mapping->memoryOffset + page_offset);
	assert(physical != PhysicalAddr(-1));

	if(p_pageSpace.isMapped(page_vaddr))
		p_pageSpace.unmapSingle4k(page_vaddr);
	p_pageSpace.mapSingle4k(physical_guard, page_vaddr, physical, true, page_flags);
	if(physical_guard.isLocked())
		physical_guard.unlock();

	return true;
}

KernelSharedPtr<AddressSpace> AddressSpace::fork(Guard &guard) {
	assert(guard.protects(&lock));

	auto forked = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());

	cloneRecursive(spaceTree.get_root(), forked.get());

	return frigg::move(forked);
}

PhysicalAddr AddressSpace::grabPhysical(Guard &guard, VirtualAddr address) {
	assert(guard.protects(&lock));
	assert((address % kPageSize) == 0);

	Mapping *mapping = getMapping(address);
	assert(mapping);
	assert(mapping->type == Mapping::kTypeMemory);

	// TODO: allocate missing pages for OnDemand or CopyOnWrite pages

	GrabIntent grab_flags = kGrabFetch | kGrabWrite;
	if(mapping->flags & Mapping::kFlagDontRequireBacking)
		grab_flags |= kGrabDontRequireBacking;

	auto page = address - mapping->baseAddress;
	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock, frigg::dontLock);
	PhysicalAddr physical = mapping->memoryRegion->grabPage(physical_guard,
			grab_flags, mapping->memoryOffset + page);
	assert(physical != PhysicalAddr(-1));
	return physical;
}

void AddressSpace::activate() {
	p_pageSpace.activate();
}

Mapping *AddressSpace::getMapping(VirtualAddr address) {
	Mapping *current = spaceTree.get_root();
	
	while(current != nullptr) {
		if(address < current->baseAddress) {
			current = SpaceTree::get_left(current);
		}else if(address >= current->baseAddress + current->length) {
			current = SpaceTree::get_right(current);
		}else{
			assert(address >= current->baseAddress
					&& address < current->baseAddress + current->length);
			return current;
		}
	}

	return nullptr;
}

Mapping *AddressSpace::allocate(size_t length, MapFlags flags) {
	assert(length > 0);
	assert((length % kPageSize) == 0);
//	frigg::infoLogger() << "Allocate virtual memory area"
//			<< ", size: 0x" << frigg::logHex(length) << frigg::endLog;

	if(spaceTree.get_root()->largestHole < length)
		return nullptr;
	
	return allocateDfs(spaceTree.get_root(), length, flags);
}

Mapping *AddressSpace::allocateDfs(Mapping *mapping, size_t length,
		MapFlags flags) {
	if((flags & kMapPreferBottom) != 0) {
		// try to allocate memory at the bottom of the range
		if(mapping->type == Mapping::kTypeHole && mapping->length >= length)
			return splitHole(mapping, 0, length);
		
		if(SpaceTree::get_left(mapping) && SpaceTree::get_left(mapping)->largestHole >= length)
			return allocateDfs(SpaceTree::get_left(mapping), length, flags);
		
		assert(SpaceTree::get_right(mapping));
		assert(SpaceTree::get_right(mapping)->largestHole >= length);
		return allocateDfs(SpaceTree::get_right(mapping), length, flags);
	}else{
		// try to allocate memory at the top of the range
		assert((flags & kMapPreferTop) != 0);
		if(mapping->type == Mapping::kTypeHole && mapping->length >= length)
			return splitHole(mapping, mapping->length - length, length);

		if(SpaceTree::get_right(mapping) && SpaceTree::get_right(mapping)->largestHole >= length)
			return allocateDfs(SpaceTree::get_right(mapping), length, flags);
		
		assert(SpaceTree::get_left(mapping));
		assert(SpaceTree::get_left(mapping)->largestHole >= length);
		return allocateDfs(SpaceTree::get_left(mapping), length, flags);
	}
}

Mapping *AddressSpace::allocateAt(VirtualAddr address, size_t length) {
	assert((address % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	Mapping *hole = getMapping(address);
	assert(hole != nullptr);
	assert(hole->type == Mapping::kTypeHole);
	
	return splitHole(hole, address - hole->baseAddress, length);
}

void AddressSpace::cloneRecursive(Mapping *mapping, AddressSpace *dest_space) {
	Mapping *dest_mapping = frigg::construct<Mapping>(*kernelAlloc, mapping->type,
			mapping->baseAddress, mapping->length);

	if(mapping->type == Mapping::kTypeHole) {
		// holes do not require additional handling
	}else if(mapping->type == Mapping::kTypeMemory
			&& (mapping->flags & Mapping::kFlagDropAtFork)) {
		// TODO: merge this hole into adjacent holes
	}else if(mapping->type == Mapping::kTypeMemory
			&& (mapping->flags & Mapping::kFlagShareAtFork)) {
		KernelUnsafePtr<Memory> memory = mapping->memoryRegion;

		uint32_t page_flags = 0;
		if(mapping->writePermission)
			page_flags |= PageSpace::kAccessWrite;
		if(mapping->executePermission)
			page_flags |= PageSpace::kAccessExecute;

		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock, frigg::dontLock);
		for(size_t page = 0; page < dest_mapping->length; page += kPageSize) {
			// TODO: do not grab pages that are unavailable
			// TODO: respect some defer-write-access flag?
			PhysicalAddr physical;
			if(mapping->writePermission) {
				physical = memory->grabPage(physical_guard, kGrabQuery | kGrabWrite,
					mapping->memoryOffset + page);
			}else{
				physical = memory->grabPage(physical_guard, kGrabQuery | kGrabRead,
					mapping->memoryOffset + page);
			}

			VirtualAddr vaddr = dest_mapping->baseAddress + page;
			if(physical != PhysicalAddr(-1))
				dest_space->p_pageSpace.mapSingle4k(physical_guard, vaddr, physical,
						true, page_flags);
		}
		if(physical_guard.isLocked())
			physical_guard.unlock();
		
		dest_mapping->memoryRegion = memory.toShared();
		dest_mapping->memoryOffset = mapping->memoryOffset;
		dest_mapping->writePermission = mapping->writePermission;
		dest_mapping->executePermission = mapping->executePermission;
	}else if(mapping->type == Mapping::kTypeMemory
			&& (mapping->flags & Mapping::kFlagCopyOnWriteAtFork)) {
		KernelUnsafePtr<Memory> memory = mapping->memoryRegion;

		// don't set the write flag here to enable copy-on-write.
		uint32_t page_flags = 0;
		if(mapping->executePermission)
			page_flags |= PageSpace::kAccessExecute;

		// Decide if we want a copy-on-write or a real copy of the mapping.
		// * Futexes attached to the memory object prevent copy-on-write.
		//     This ensures that processes do not miss wake ups that would otherwise
		//     hit the copy-on-write memory object.
		KernelSharedPtr<Memory> dest_memory;
		if(memory->futex.empty()) {
			// Create a copy-on-write object for the original space.
			auto src_memory = frigg::makeShared<CopyOnWriteMemory>(*kernelAlloc,
					memory.toShared());
			{
				PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
				for(size_t page = 0; page < mapping->length; page += kPageSize) {
					PhysicalAddr physical = src_memory->grabPage(physical_guard,
							kGrabQuery | kGrabRead, mapping->memoryOffset + page);
					assert(physical != PhysicalAddr(-1));
					VirtualAddr vaddr = mapping->baseAddress + page;
					p_pageSpace.unmapSingle4k(vaddr);
					p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
				}
			}
			mapping->memoryRegion = frigg::move(src_memory);
			
			// Create a copy-on-write region for the forked space
			dest_memory = frigg::makeShared<CopyOnWriteMemory>(*kernelAlloc,
					memory.toShared());
		}else{
			// We perform an actual copy so we can grant write permission.
			if(mapping->writePermission)
				page_flags |= PageSpace::kAccessWrite;

			// Perform a real copy operation here.
			dest_memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc,
					memory->getLength(), kPageSize, kPageSize);
			for(size_t page = 0; page < memory->getLength(); page += kPageSize) {
				PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
				PhysicalAddr src_physical = memory->grabPage(physical_guard,
						kGrabQuery | kGrabRead, page);
				PhysicalAddr dest_physical = dest_memory->grabPage(physical_guard,
						kGrabFetch | kGrabWrite, page);
				assert(src_physical != PhysicalAddr(-1));
				assert(dest_physical != PhysicalAddr(-1));
		
				PageAccessor dest_accessor{generalWindow, dest_physical};
				PageAccessor src_accessor{generalWindow, src_physical};
				memcpy(dest_accessor.get(), src_accessor.get(), kPageSize);
			}
		}

		// Finally we map the new memory object to the cloned address space.
		{
			PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
			for(size_t page = 0; page < mapping->length; page += kPageSize) {
				PhysicalAddr physical = dest_memory->grabPage(physical_guard,
						kGrabQuery | kGrabRead, mapping->memoryOffset + page);
				assert(physical != PhysicalAddr(-1));
				VirtualAddr vaddr = mapping->baseAddress + page;
				dest_space->p_pageSpace.mapSingle4k(physical_guard, vaddr, physical,
						true, page_flags);
			}
		}

		dest_mapping->memoryRegion = frigg::move(dest_memory);
		dest_mapping->writePermission = mapping->writePermission;
		dest_mapping->executePermission = mapping->executePermission;
		dest_mapping->memoryOffset = mapping->memoryOffset;
	}else{
		assert(!"Illegal mapping type");
	}

	dest_space->spaceTree.insert(dest_mapping);

	if(SpaceTree::get_left(mapping))
		cloneRecursive(SpaceTree::get_left(mapping), dest_space);
	if(SpaceTree::get_right(mapping))
		cloneRecursive(SpaceTree::get_right(mapping), dest_space);
}

Mapping *AddressSpace::splitHole(Mapping *mapping,
		VirtualAddr split_offset, size_t split_length) {
	assert(split_length > 0);
	assert(mapping->type == Mapping::kTypeHole);
	assert(split_offset + split_length <= mapping->length);
	
	VirtualAddr hole_address = mapping->baseAddress;
	size_t hole_length = mapping->length;
	
	if(split_offset == 0) {
		// the split mapping starts at the beginning of the hole
		// we have to delete the hole mapping
		spaceTree.remove(mapping);
		frigg::destruct(*kernelAlloc, mapping);
	}else{
		// the split mapping starts in the middle of the hole
		mapping->length = split_offset;
		spaceTree.aggregate_path(mapping);
	}

	auto split = frigg::construct<Mapping>(*kernelAlloc, Mapping::kTypeNone,
			hole_address + split_offset, split_length);
	spaceTree.insert(split);

	if(hole_length > split_offset + split_length) {
		// the split mapping does not go on until the end of the hole
		// we have to create another mapping for the rest of the hole
		auto following = frigg::construct<Mapping>(*kernelAlloc, Mapping::kTypeHole,
				hole_address + (split_offset + split_length),
				hole_length - (split_offset + split_length));
		spaceTree.insert(following);
	}else{
		assert(hole_length == split_offset + split_length);
	}

	return split;
}

} // namespace thor

