
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// HardwareMemory
// --------------------------------------------------------

HardwareMemory::HardwareMemory(PhysicalAddr base, size_t length)
: _base(base), _length(length) {
	assert(base % kPageSize == 0);
	assert(length % kPageSize == 0);
}

size_t HardwareMemory::getLength() {
	return _length;
}

PhysicalAddr HardwareMemory::grabPage(PhysicalChunkAllocator::Guard &physical_guard,
		GrabIntent grab_intent, size_t offset) {
	assert(offset % kPageSize == 0);
	assert(offset + kPageSize <= _length);
	return _base + offset;
}

// --------------------------------------------------------
// AllocatedMemory
// --------------------------------------------------------

AllocatedMemory::AllocatedMemory(size_t length, size_t chunk_size, size_t chunk_align)
: _physicalChunks(*kernelAlloc), _chunkSize(chunk_size), _chunkAlign(chunk_align) {
	assert(_chunkSize % kPageSize == 0);
	assert(_chunkAlign % kPageSize == 0);
	assert(_chunkSize % _chunkAlign == 0);
	assert(length % _chunkSize == 0);
	_physicalChunks.resize(length / _chunkSize, PhysicalAddr(-1));
}

size_t AllocatedMemory::getLength() {
	return _physicalChunks.size() * _chunkSize;
}

PhysicalAddr AllocatedMemory::grabPage(PhysicalChunkAllocator::Guard &physical_guard,
		GrabIntent grab_intent, size_t offset) {
	assert(offset % kPageSize == 0);
	
	size_t index = offset / _chunkSize;
	size_t disp = offset % _chunkSize;
	assert(index < _physicalChunks.size());

	if(_physicalChunks[index] == PhysicalAddr(-1)) {
		if(!physical_guard.isLocked())
			physical_guard.lock();
		PhysicalAddr physical = physicalAllocator->allocate(physical_guard, _chunkSize);
		assert(physical % _chunkAlign == 0);
		memset(physicalToVirtual(physical), 0, _chunkSize);
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

void ManagedSpace::progressLoads() {
	// TODO: this function could issue loads > a single kPageSize
	while(!initiateLoadQueue.empty()) {
		frigg::UnsafePtr<AsyncInitiateLoad> initiate = initiateLoadQueue.front();

		size_t index = (initiate->offset + initiate->progress) / kPageSize;
		if(loadState[index] == kStateMissing) {
			if(handleLoadQueue.empty())
				break;

			loadState[index] = kStateLoading;

			frigg::SharedPtr<AsyncHandleLoad> handle = handleLoadQueue.removeFront();
			handle->offset = initiate->offset + initiate->progress;
			handle->length = kPageSize;
			AsyncOperation::complete(frigg::move(handle));

			initiate->progress += kPageSize;
		}else if(loadState[index] == kStateLoading) {
			initiate->progress += kPageSize;
		}else{
			assert(loadState[index] == kStateLoaded);
			initiate->progress += kPageSize;
		}

		if(initiate->progress == initiate->length) {
			if(isComplete(initiate)) {
				AsyncOperation::complete(initiateLoadQueue.removeFront());
			}else{
				pendingLoadQueue.addBack(initiateLoadQueue.removeFront());
			}
		}
	}
}

bool ManagedSpace::isComplete(frigg::UnsafePtr<AsyncInitiateLoad> initiate) {
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
		GrabIntent grab_intent, size_t offset) {
	assert(offset % kPageSize == 0);
	
	size_t index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());

	if(_managed->physicalPages[index] == PhysicalAddr(-1)) {
		if(!physical_guard.isLocked())
			physical_guard.lock();
		PhysicalAddr physical = physicalAllocator->allocate(physical_guard, kPageSize);
		memset(physicalToVirtual(physical), 0, kPageSize);
		_managed->physicalPages[index] = physical;
	}

	return _managed->physicalPages[index];
}

void BackingMemory::submitHandleLoad(frigg::SharedPtr<AsyncHandleLoad> handle) {
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
		if(_managed->isComplete(*it_copy))
			AsyncOperation::complete(_managed->pendingLoadQueue.remove(it_copy));
	}
}

// --------------------------------------------------------
// FrontalMemory
// --------------------------------------------------------

size_t FrontalMemory::getLength() {
	return _managed->physicalPages.size() * kPageSize;
}

PhysicalAddr FrontalMemory::grabPage(PhysicalChunkAllocator::Guard &physical_guard,
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

			struct NullAllocator {
				void free(void *) { }
			};
			NullAllocator null_allocator;

			KernelUnsafePtr<Thread> this_thread = getCurrentThread();
			frigg::SharedBlock<AsyncInitiateLoad, NullAllocator> block(null_allocator,
					ReturnFromForkCompleter(this_thread.toWeak()), offset, kPageSize);
			frigg::SharedPtr<AsyncInitiateLoad> initiate(frigg::adoptShared, &block);
			
			Thread::blockCurrent([&] () {
				_managed->initiateLoadQueue.addBack(frigg::move(initiate));
				_managed->progressLoads();
			});
		}

		PhysicalAddr physical = _managed->physicalPages[index];
		assert(physical != PhysicalAddr(-1));
		return physical;
	}
}

void FrontalMemory::submitInitiateLoad(frigg::SharedPtr<AsyncInitiateLoad> initiate) {
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
: _origin(frigg::move(origin)), _physicalPages(*kernelAlloc) {
	assert(_origin->getLength() % kPageSize == 0);
	_physicalPages.resize(_origin->getLength() / kPageSize, PhysicalAddr(-1));
}

size_t CopyOnWriteMemory::getLength() {
	return _physicalPages.size() * kPageSize;
}

PhysicalAddr CopyOnWriteMemory::grabPage(PhysicalChunkAllocator::Guard &physical_guard,
		GrabIntent grab_intent, size_t offset) {
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
		memcpy(physicalToVirtual(own_physical), physicalToVirtual(origin_physical), kPageSize);
		_physicalPages[index] = own_physical;
	}

	return _physicalPages[index];
}

// --------------------------------------------------------
// Memory
// --------------------------------------------------------

Memory::Memory(MemoryVariant variant)
: _variant(frigg::move(variant)) { }

void Memory::copyFrom(size_t offset, void *source, size_t length) {
	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock,
			frigg::dontLock);
	
	size_t progress = 0;

	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, length);
		PhysicalAddr page = grabPage(physical_guard, kGrabFetch | kGrabWrite, offset - misalign);
		assert(page != PhysicalAddr(-1));
		memcpy((uint8_t *)physicalToVirtual(page) + misalign, source, prefix);
		progress += prefix;
	}

	while(length - progress >= kPageSize) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = grabPage(physical_guard, kGrabFetch | kGrabWrite, offset + progress);
		assert(page != PhysicalAddr(-1));
		memcpy(physicalToVirtual(page), (uint8_t *)source + progress, kPageSize);
		progress += kPageSize;
	}

	if(length - progress > 0) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = grabPage(physical_guard, kGrabFetch | kGrabWrite, offset + progress);
		assert(page != PhysicalAddr(-1));
		memcpy(physicalToVirtual(page), (uint8_t *)source + progress, length - progress);
	}
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
	if(mapping->executePermission);
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

		// don't set the write flag to enable copy-on-write
		uint32_t page_flags = 0;
		if(mapping->executePermission);
			page_flags |= PageSpace::kAccessExecute;
		
		// create a copy-on-write region for the original space
		auto src_memory = frigg::makeShared<Memory>(*kernelAlloc,
				CopyOnWriteMemory(memory.toShared()));
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
		
		// create a copy-on-write region for the forked space
		auto dest_memory = frigg::makeShared<Memory>(*kernelAlloc,
				CopyOnWriteMemory(memory.toShared()));
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

