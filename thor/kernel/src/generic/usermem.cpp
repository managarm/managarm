
#include <type_traits>
#include "kernel.hpp"
#include <frg/container_of.hpp>
#include "types.hpp"

namespace thor {

namespace {
	frigg::Tuple<uintptr_t, size_t> alignRange(uintptr_t offset, size_t length, size_t align) {
		auto misalign = offset & (align - 1);
		return frigg::Tuple<uintptr_t, size_t>{offset - misalign,
				(misalign + length + (align - 1)) & ~(align - 1)};
	}
}

// --------------------------------------------------------

CowBundle::CowBundle(frigg::SharedPtr<VirtualView> view, ptrdiff_t offset, size_t size)
: _superRoot{frigg::move(view)}, _superOffset{offset}, _pages{kernelAlloc.get()} {
	assert(!(size & (kPageSize - 1)));
	_copy = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, kPageSize, kPageSize);
}

CowBundle::CowBundle(frigg::SharedPtr<CowBundle> chain, ptrdiff_t offset, size_t size)
: _superChain{frigg::move(chain)}, _superOffset{offset}, _pages{kernelAlloc.get()} {
	assert(!(size & (kPageSize - 1)));
	_copy = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, kPageSize, kPageSize);
}

PhysicalAddr CowBundle::fetchRange(uintptr_t offset) {
	assert(!(offset & (kPageSize - 1)));

	// If the page is present in this bundle we just return it.
	if(auto it = _pages.find(offset >> kPageShift); it) {
		auto physical = it->load(std::memory_order_relaxed);
		assert(physical != PhysicalAddr(-1));
		return physical;
	}

	// Otherwise we need to copy from the super-tree.
	CowBundle *chain = this;
	ptrdiff_t disp = offset;
	while(true) {
		// Copy from a descendant CoW bundle.
		if(auto it = chain->_pages.find(disp >> kPageShift); it) {
			// Cannot copy from ourselves; this case is handled above.
			assert(chain != this);
			Memory::transfer(_copy.get(), offset, chain->_copy.get(), disp, kPageSize);

			auto physical = _copy->fetchRange(offset);
			assert(physical != PhysicalAddr(-1));
			auto cow_it = _pages.insert(offset >> kPageShift, PhysicalAddr(-1));
			cow_it->store(physical, std::memory_order_relaxed);
			return physical;
		}

		// Copy from the root view.
		if(!chain->_superChain) {
			assert(chain->_superRoot);
			auto bundle = chain->_superRoot->resolveRange(chain->_superOffset + disp, kPageSize);
			Memory::transfer(_copy.get(), offset, bundle.get<0>(), bundle.get<1>(), kPageSize);

			auto physical = _copy->fetchRange(offset);
			assert(physical != PhysicalAddr(-1));
			auto cow_it = _pages.insert(offset >> kPageShift, PhysicalAddr(-1));
			cow_it->store(physical, std::memory_order_relaxed);
			return physical;
		}

		disp += chain->_superOffset;
		chain = chain->_superChain.get();
	}
}

// --------------------------------------------------------
// Memory
// --------------------------------------------------------

void Memory::transfer(MemoryBundle *dest_memory, uintptr_t dest_offset,
		MemoryBundle *src_memory, uintptr_t src_offset, size_t length) {
//	dest_memory->acquire(dest_offset, length);
//	src_memory->acquire(src_offset, length);

	size_t progress = 0;
	while(progress < length) {
		auto dest_misalign = (dest_offset + progress) % kPageSize;
		auto src_misalign = (src_offset + progress) % kPageSize;
		size_t chunk = frigg::min(frigg::min(kPageSize - dest_misalign,
				kPageSize - src_misalign), length - progress);

		PhysicalAddr dest_page = dest_memory->fetchRange(dest_offset + progress - dest_misalign);
		PhysicalAddr src_page = src_memory->fetchRange(src_offset + progress - dest_misalign);
		assert(dest_page != PhysicalAddr(-1));
		assert(src_page != PhysicalAddr(-1));

		PageAccessor dest_accessor{generalWindow, dest_page};
		PageAccessor src_accessor{generalWindow, src_page};
		memcpy((uint8_t *)dest_accessor.get() + dest_misalign,
				(uint8_t *)src_accessor.get() + src_misalign, chunk);

		progress += chunk;
	}

//	dest_memory->release(dest_offset, length);
//	src_memory->release(src_offset, length);
}

void Memory::copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t size) {
	(void)offset;
	(void)pointer;
	(void)size;
	frigg::panicLogger() << "Bundle does not support synchronous operations!" << frigg::endLog;
}

void Memory::resize(size_t new_length) {
	(void)new_length;
	frigg::panicLogger() << "Bundle does not support resize!" << frigg::endLog;
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

// --------------------------------------------------------
// Copy operations.
// --------------------------------------------------------

void copyToBundle(Memory *bundle, ptrdiff_t offset, const void *pointer, size_t size,
		CopyToBundleNode *node, void (*complete)(CopyToBundleNode *)) {
	bundle->acquire(offset, size);

	size_t progress = 0;
	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, size);
		PhysicalAddr page = bundle->fetchRange(offset - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{generalWindow, page};
		memcpy((uint8_t *)accessor.get() + misalign, pointer, prefix);
		progress += prefix;
	}

	while(size - progress >= kPageSize) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = bundle->fetchRange(offset + progress);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{generalWindow, page};
		memcpy(accessor.get(), (uint8_t *)pointer + progress, kPageSize);
		progress += kPageSize;
	}

	if(size - progress > 0) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = bundle->fetchRange(offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{generalWindow, page};
		memcpy(accessor.get(), (uint8_t *)pointer + progress, size - progress);
	}

	bundle->release(offset, size);
	complete(node);
}

void copyFromBundle(Memory *bundle, ptrdiff_t offset, void *buffer, size_t size,
		CopyFromBundleNode *node, void (*complete)(CopyFromBundleNode *)) {
	bundle->acquire(offset, size);

	size_t progress = 0;
	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, size);
		PhysicalAddr page = bundle->fetchRange(offset - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{generalWindow, page};
		memcpy(buffer, (uint8_t *)accessor.get() + misalign, prefix);
		progress += prefix;
	}

	while(size - progress >= kPageSize) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = bundle->fetchRange(offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{generalWindow, page};
		memcpy((uint8_t *)buffer + progress, accessor.get(), kPageSize);
		progress += kPageSize;
	}

	if(size - progress > 0) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = bundle->fetchRange(offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{generalWindow, page};
		memcpy((uint8_t *)buffer + progress, accessor.get(), size - progress);
	}

	bundle->release(offset, size);
	complete(node);
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

AllocatedMemory::AllocatedMemory(size_t desired_length, size_t desired_chunk_size,
		size_t chunk_align)
: Memory(MemoryTag::allocated), _physicalChunks(*kernelAlloc), _chunkAlign(chunk_align) {
	static_assert(sizeof(unsigned long) == sizeof(uint64_t), "Fix use of __builtin_clzl");
	_chunkSize = size_t(1) << (64 - __builtin_clzl(desired_chunk_size - 1));
	if(_chunkSize != desired_chunk_size)
		frigg::infoLogger() << "\e[31mPhysical allocation of size " << (void *)desired_chunk_size
				<< " rounded up to power of 2\e[39m" << frigg::endLog;

	size_t length = (desired_length + (_chunkSize - 1)) & ~(_chunkSize - 1);
	if(length != desired_length)
		frigg::infoLogger() << "\e[31mMemory length " << (void *)desired_length
				<< " rounded up to chunk size " << (void *)_chunkSize
				<< "\e[39m" << frigg::endLog;

	assert(_chunkSize % kPageSize == 0);
	assert(_chunkAlign % kPageSize == 0);
	assert(_chunkSize % _chunkAlign == 0);
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

void AllocatedMemory::resize(size_t new_length) {
	assert(!(new_length % _chunkSize));
	size_t num_chunks = new_length / _chunkSize;
	assert(num_chunks >= _physicalChunks.size());
	_physicalChunks.resize(num_chunks, PhysicalAddr(-1));
}

void AllocatedMemory::copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t size) {
	// TODO: For now we only allow naturally aligned access.
	assert(size <= kPageSize);
	assert(!(offset % size));

	size_t index = offset / _chunkSize;
	assert(index < _physicalChunks.size());
	if(_physicalChunks[index] == PhysicalAddr(-1)) {
		auto physical = physicalAllocator->allocate(_chunkSize);
		assert(physical != PhysicalAddr(-1));
		assert(!(physical % _chunkAlign));

		for(size_t pg_progress = 0; pg_progress < _chunkSize; pg_progress += kPageSize) {
			PageAccessor accessor{generalWindow, physical + pg_progress};
			memset(accessor.get(), 0, kPageSize);
		}
		_physicalChunks[index] = physical;
	}

	PageAccessor accessor{generalWindow, _physicalChunks[index]
			+ ((offset % _chunkSize) / kPageSize)};
	memcpy((uint8_t *)accessor.get() + (offset % kPageSize), pointer, size);
}

void AllocatedMemory::acquire(uintptr_t offset, size_t length) {
	// TODO: Mark the pages as locked.
	(void)offset;
	(void)length;
}

void AllocatedMemory::release(uintptr_t offset, size_t length) {
	// TODO: Mark the pages as unlocked.
	(void)offset;
	(void)length;
}

PhysicalAddr AllocatedMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	auto index = offset / _chunkSize;
	auto misalign = offset & (_chunkSize - 1);
	assert(index < _physicalChunks.size());
	if(_physicalChunks[index] == PhysicalAddr(-1))
		return PhysicalAddr(-1);
	return _physicalChunks[index] + misalign;
}

PhysicalAddr AllocatedMemory::fetchRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);

	auto range = alignRange(offset, kPageSize, _chunkSize);
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

	auto index = offset / _chunkSize;
	auto misalign = offset & (_chunkSize - 1);
	assert(index < _physicalChunks.size());
	assert(_physicalChunks[index] != PhysicalAddr(-1));
	return _physicalChunks[index] + misalign;
}

size_t AllocatedMemory::getLength() {
	return _physicalChunks.size() * _chunkSize;
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
	assert(!(offset % kPageSize));

	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	return _managed->physicalPages[index];
}

PhysicalAddr BackingMemory::fetchRange(uintptr_t offset) {
	assert(!(offset % kPageSize));
	
	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	if(_managed->physicalPages[index] == PhysicalAddr(-1)) {
		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		
		PageAccessor accessor{generalWindow, physical};
		memset(accessor.get(), 0, kPageSize);
		_managed->physicalPages[index] = physical;
	}

	return _managed->physicalPages[index];
}

size_t BackingMemory::getLength() {
	return _managed->physicalPages.size() * kPageSize;
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
	assert(!(offset % kPageSize));

	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	if(_managed->loadState[index] != ManagedSpace::kStateLoaded)
		return PhysicalAddr(-1);
	return _managed->physicalPages[index];
}

PhysicalAddr FrontalMemory::fetchRange(uintptr_t offset) {
	assert(!(offset % kPageSize));

	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	if(_managed->loadState[index] != ManagedSpace::kStateLoaded) {
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

//		frigg::infoLogger() << "thor: Thread blocked on memory read" << frigg::endLog;
		Thread::blockCurrentWhile([&] {
			return !complete.load(std::memory_order_acquire);
		});
	}

	auto physical = _managed->physicalPages[index];
	assert(physical != PhysicalAddr(-1));
	return physical;
}

size_t FrontalMemory::getLength() {
	return _managed->physicalPages.size() * kPageSize;
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

ExteriorBundleView::ExteriorBundleView(frigg::SharedPtr<MemoryBundle> bundle,
		ptrdiff_t view_offset, size_t view_size)
: _bundle{frigg:move(bundle)}, _viewOffset{view_offset}, _viewSize{view_size} { }

frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
ExteriorBundleView::resolveRange(ptrdiff_t offset, size_t size) {
	assert(offset + size <= _viewSize);
	return frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>{_bundle.get(), _viewOffset + offset,
			frigg::min(size, _viewSize - offset)};
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

Mapping::Mapping(AddressSpace *owner, VirtualAddr base_address, size_t length,
		MappingFlags flags)
: _owner{owner}, _address{base_address}, _length{length}, _flags{flags} { }

// --------------------------------------------------------
// NormalMapping
// --------------------------------------------------------

NormalMapping::NormalMapping(AddressSpace *owner, VirtualAddr address, size_t length,
		MappingFlags flags, frigg::SharedPtr<Memory> memory, uintptr_t offset)
: Mapping{owner, address, length, flags}, _memory{frigg::move(memory)}, _offset{offset} { }

frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
NormalMapping::resolveRange(ptrdiff_t offset, size_t size) {
	assert(offset + size <= length());
	return frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>{_memory.get(), _offset + offset,
			frigg::min(size, length() - offset)};
}

Mapping *NormalMapping::shareMapping(AddressSpace *dest_space) {
	// TODO: Always keep the exact flags?
	return frigg::construct<NormalMapping>(*kernelAlloc, dest_space,
			address(), length(), flags(), _memory, _offset);
}

Mapping *NormalMapping::copyOnWrite(AddressSpace *dest_space) {
	auto chain = frigg::makeShared<CowBundle>(*kernelAlloc, 
			frigg::makeShared<ExteriorBundleView>(*kernelAlloc, _memory, 0, _memory->getLength()),
			 _offset, length());
	return frigg::construct<CowMapping>(*kernelAlloc, dest_space,
			address(), length(), flags(), frigg::move(chain));
}

void NormalMapping::install(bool overwrite) {
	uint32_t page_flags = 0;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)
		page_flags |= page_access::write;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)
		page_flags |= page_access::execute;
	// TODO: Allow inaccessible mappings.
	assert((flags() & MappingFlags::permissionMask) & MappingFlags::protRead);

	for(size_t progress = 0; progress < length(); progress += kPageSize) {
		// TODO: Add a don't-require-backing flag to peekRange.
		//if(flags() & MappingFlags::dontRequireBacking)
		//	grab_flags |= kGrabDontRequireBacking;

		PhysicalAddr physical = _memory->peekRange(_offset + progress);

		VirtualAddr vaddr = address() + progress;
		if(overwrite && owner()->_pageSpace.isMapped(vaddr)) {
			owner()->_pageSpace.unmapRange(vaddr, kPageSize, PageMode::normal);
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

	owner()->_pageSpace.unmapRange(address(), length(), PageMode::remap);
}

PhysicalAddr NormalMapping::grabPhysical(VirtualAddr disp) {
	// TODO: Allocate missing pages for OnDemand or CopyOnWrite pages.

	// TODO: Add a don't-require-backing flag to peekRange.
	//if(flags() & MappingFlags::dontRequireBacking)
	//	grab_flags |= kGrabDontRequireBacking;

	return _memory->fetchRange(_offset + disp);
}

bool NormalMapping::handleFault(VirtualAddr disp, uint32_t fault_flags) {
	if(fault_flags & AddressSpace::kFaultWrite)
		if(!((flags() & MappingFlags::permissionMask) & MappingFlags::protWrite))
			return false;
	if(fault_flags & AddressSpace::kFaultExecute)
		if(!((flags() & MappingFlags::permissionMask) & MappingFlags::protExecute))
			return false;

	uint32_t page_flags = 0;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)
		page_flags |= page_access::write;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)
		page_flags |= page_access::execute;
	// TODO: Allow inaccessible mappings.
	assert((flags() & MappingFlags::permissionMask) & MappingFlags::protRead);

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
		MappingFlags flags, frigg::SharedPtr<CowBundle> chain)
: Mapping{owner, address, length, flags}, _cowBundle{frigg::move(chain)} {
}

frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
CowMapping::resolveRange(ptrdiff_t offset, size_t size) {
	assert(offset + size <= length());
	return frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>{_cowBundle.get(), offset,
			frigg::min(size, length())};
}

Mapping *CowMapping::shareMapping(AddressSpace *dest_space) {
	(void)dest_space;
	assert(!"Fix this");
	__builtin_unreachable();
}

Mapping *CowMapping::copyOnWrite(AddressSpace *dest_space) {
	auto sub_chain = frigg::makeShared<CowBundle>(*kernelAlloc, _cowBundle, 0, length());
	return frigg::construct<CowMapping>(*kernelAlloc, dest_space,
			address(), length(), flags(), frigg::move(sub_chain));
}

void CowMapping::install(bool overwrite) {
	// For now we just unmap everything. TODO: Map available pages.
	for(size_t progress = 0; progress < length(); progress += kPageSize) {
		VirtualAddr vaddr = address() + progress;
		if(overwrite && owner()->_pageSpace.isMapped(vaddr)) {
			owner()->_pageSpace.unmapRange(vaddr, kPageSize, PageMode::normal);
		}else{
			assert(!owner()->_pageSpace.isMapped(vaddr));
		}
	}
}

void CowMapping::uninstall(bool clear) {
	if(!clear)
		return;

	owner()->_pageSpace.unmapRange(address(), length(), PageMode::remap);
}

PhysicalAddr CowMapping::grabPhysical(VirtualAddr disp) {
	return _cowBundle->fetchRange(disp);
}

bool CowMapping::handleFault(VirtualAddr fault_offset, uint32_t fault_flags) {
	// TODO: We do not need to copy on read.
	if(fault_flags & AddressSpace::kFaultWrite)
		if(!((flags() & MappingFlags::permissionMask) & MappingFlags::protWrite))
			return false;
	if(fault_flags & AddressSpace::kFaultExecute)
		if(!((flags() & MappingFlags::permissionMask) & MappingFlags::protExecute))
			return false;

	uint32_t page_flags = 0;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)
		page_flags |= page_access::write;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)
		page_flags |= page_access::execute;
	// TODO: Allow inaccessible mappings.
	assert((flags() & MappingFlags::permissionMask) & MappingFlags::protRead);

	auto fault_page = fault_offset & ~(kPageSize - 1);
	auto physical = _cowBundle->fetchRange(fault_page);
	// TODO: Ensure that no racing threads still see the original page.
	owner()->_pageSpace.mapSingle4k(address() + fault_page, physical,
			true, page_flags);
	return true;
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------



// --------------------------------------------------------

AddressSpace::AddressSpace() { }

AddressSpace::~AddressSpace() {
	assert(!"Fix this");
	// TODO: fix this by iteratively freeing all nodes of *_mappings*.
}

void AddressSpace::setupDefaultMappings() {
	auto hole = frigg::construct<Hole>(*kernelAlloc, 0x100000, 0x7ffffff00000);
	_holes.insert(hole);
}

void AddressSpace::map(Guard &guard,
		frigg::UnsafePtr<Memory> memory, VirtualAddr address,
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

	auto mapping = frigg::construct<NormalMapping>(*kernelAlloc, this, target, length,
			static_cast<MappingFlags>(mapping_flags), memory.toShared(), offset);
	
	// Install the new mapping object.
	_mappings.insert(mapping);
	assert(!(flags & kMapPopulate));
	mapping->install(false);

	*actual_address = target;
}

void AddressSpace::unmap(Guard &guard, VirtualAddr address, size_t length,
		AddressUnmapNode *node) {
	assert(guard.protects(&lock));

	Mapping *mapping = _getMapping(address);
	assert(mapping);

	// TODO: Allow shrinking of the mapping.
	assert(mapping->address() == address);
	assert(mapping->length() == length);
	mapping->uninstall(true);

	_mappings.remove(mapping);
	frigg::destruct(*kernelAlloc, mapping);

	node->_shootNode.shotDown = [] (ShootNode *sn) {
		auto node = frg::container_of(sn, &AddressUnmapNode::_shootNode);

		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&node->_space->lock);

		// Find the holes that preceede/succeede mapping.
		Hole *pre;
		Hole *succ;

		auto current = node->_space->_holes.get_root();
		while(true) {
			assert(current);
			if(sn->address < current->address()) {
				if(HoleTree::get_left(current)) {
					current = HoleTree::get_left(current);
				}else{
					pre = HoleTree::predecessor(current);
					succ = current;
					break;
				}
			}else{
				assert(sn->address >= current->address() + current->length());
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
		if(pre && pre->address() + pre->length() == sn->address
				&& succ && sn->address + sn->size == succ->address()) {
			auto hole = frigg::construct<Hole>(*kernelAlloc, pre->address(),
					pre->length() + sn->size + succ->length());

			node->_space->_holes.remove(pre);
			node->_space->_holes.remove(succ);
			node->_space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, pre);
			frigg::destruct(*kernelAlloc, succ);
		}else if(pre && pre->address() + pre->length() == sn->address) {
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					pre->address(), pre->length() + sn->size);

			node->_space->_holes.remove(pre);
			node->_space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, pre);
		}else if(succ && sn->address + sn->size == succ->address()) {
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					sn->address, sn->size + succ->length());

			node->_space->_holes.remove(succ);
			node->_space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, succ);
		}else{
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					sn->address, sn->size);

			node->_space->_holes.insert(hole);
		}
	};

	node->_space = this;
	node->_shootNode.address = address;
	node->_shootNode.size = length;

	// Work around a deadlock if submitShootdown() invokes shotDown() immediately.
	// TODO: This should probably be resolved by running shotDown() from some callback queue.
	guard.unlock();

	_pageSpace.submitShootdown(&node->_shootNode);
}

bool AddressSpace::handleFault(VirtualAddr address, uint32_t fault_flags) {
	// TODO: It seems that this is not invoked for on-demand allocation
	// of AllocatedMemory objects!

	Mapping *mapping;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&lock);

		mapping = _getMapping(address);
		if(!mapping)
			return false;
	}

	// FIXME: mapping might be deleted here!
	// We need to use either refcounting or QS garbage collection here!

	return mapping->handleFault(address - mapping->address(), fault_flags);
}

frigg::SharedPtr<AddressSpace> AddressSpace::fork(Guard &guard) {
	assert(guard.protects(&lock));

	auto fork_space = frigg::makeShared<AddressSpace>(*kernelAlloc);

	// Copy holes to the child space.
	auto cur_hole = _holes.first();
	while(cur_hole) {
		auto fork_hole = frigg::construct<Hole>(*kernelAlloc,
				cur_hole->address(), cur_hole->length());
		fork_space->_holes.insert(fork_hole);

		cur_hole = HoleTree::successor(cur_hole);
	}

	// Modify memory mapping of both spaces.
	auto cur_mapping = _mappings.first();
	while(cur_mapping) {
		auto successor = MappingTree::successor(cur_mapping);

		if(cur_mapping->flags() & MappingFlags::dropAtFork) {
			// TODO: Merge this hole into adjacent holes.
			auto fork_hole = frigg::construct<Hole>(*kernelAlloc,
					cur_mapping->address(), cur_mapping->length());
			fork_space->_holes.insert(fork_hole);
		}else if(cur_mapping->flags() & MappingFlags::shareAtFork) {
			auto fork_mapping = cur_mapping->shareMapping(fork_space.get());

			fork_space->_mappings.insert(fork_mapping);
			fork_mapping->install(false);
		}else if(cur_mapping->flags() & MappingFlags::copyOnWriteAtFork) {
			// TODO: Copy-on-write if possible and plain copy otherwise.
			// TODO: Decide if we want a copy-on-write or a real copy of the mapping.
			// * Pinned mappings prevent CoW.
			//     This is necessary because CoW may change mapped pages
			//     in the original space.
			// * Futexes attached to the memory object prevent CoW.
			//     This ensures that processes do not miss wake ups in the original space.
			auto origin_mapping = cur_mapping->copyOnWrite(this);
			auto fork_mapping = cur_mapping->copyOnWrite(fork_space.get());

			_mappings.remove(cur_mapping);
			_mappings.insert(origin_mapping);
			fork_space->_mappings.insert(fork_mapping);
			cur_mapping->uninstall(false);
			origin_mapping->install(true);
			fork_mapping->install(false);
			frigg::destruct(*kernelAlloc, cur_mapping);
		}else{
			assert(!"Illegal mapping type");
		}

		cur_mapping = successor;
	}

	return frigg::move(fork_space);
}

PhysicalAddr AddressSpace::grabPhysical(Guard &guard, VirtualAddr address) {
	assert(guard.protects(&lock));
	assert((address % kPageSize) == 0);

	Mapping *mapping = _getMapping(address);
	if(!mapping)
		return PhysicalAddr(-1);
	return mapping->grabPhysical(address - mapping->address());
}

void AddressSpace::activate() {
	_pageSpace.activate();
}

Mapping *AddressSpace::_getMapping(VirtualAddr address) {
	auto current = _mappings.get_root();
	while(current) {
		if(address < current->address()) {
			current = MappingTree::get_left(current);
		}else if(address >= current->address() + current->length()) {
			current = MappingTree::get_right(current);
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

VirtualAddr AddressSpace::_allocateAt(VirtualAddr address, size_t length) {
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

void AddressSpace::_splitHole(Hole *hole, VirtualAddr offset, size_t length) {
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

} // namespace thor

