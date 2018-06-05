
#include <type_traits>
#include "kernel.hpp"
#include <frg/container_of.hpp>
#include "types.hpp"

namespace thor {

PhysicalAddr MemoryBundle::blockForRange(uintptr_t offset) {
	struct Node : FetchNode {
		Node()
		: blockedThread{getCurrentThread()}, complete{false} { }

		frigg::UnsafePtr<Thread> blockedThread;
		std::atomic<bool> complete;
	} node;

	auto functor = [] (FetchNode *base) {
		auto np = static_cast<Node *>(base);
		np->complete.store(true, std::memory_order_release);
		Thread::unblockOther(np->blockedThread);
	};

	if(!fetchRange(offset, &node, functor)) 
		Thread::blockCurrentWhile([&] {
			return !node.complete.load(std::memory_order_acquire);
		});
	return node.range().get<0>();
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

PhysicalAddr CowBundle::peekRange(uintptr_t) {
	assert(!"This should never be called");
	__builtin_unreachable();
}

bool CowBundle::fetchRange(uintptr_t offset, FetchNode *node, void (*fetched)(FetchNode *)) {
	assert(!(offset & (kPageSize - 1)));
	setupFetch(node, fetched);

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	// If the page is present in this bundle we just return it.
	if(auto it = _pages.find(offset >> kPageShift); it) {
		auto physical = it->load(std::memory_order_relaxed);
		assert(physical != PhysicalAddr(-1));
		completeFetch(node, physical, kPageSize);
		return true;
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

			auto physical = _copy->blockForRange(offset);
			assert(physical != PhysicalAddr(-1));
			auto cow_it = _pages.insert(offset >> kPageShift, PhysicalAddr(-1));
			cow_it->store(physical, std::memory_order_relaxed);
			completeFetch(node, physical, kPageSize);
			return true;
		}

		// Copy from the root view.
		if(!chain->_superChain) {
			assert(chain->_superRoot);
			auto bundle = chain->_superRoot->resolveRange(chain->_superOffset + disp, kPageSize);
			Memory::transfer(_copy.get(), offset, bundle.get<0>(), bundle.get<1>(), kPageSize);

			auto physical = _copy->blockForRange(offset);
			assert(physical != PhysicalAddr(-1));
			auto cow_it = _pages.insert(offset >> kPageShift, PhysicalAddr(-1));
			cow_it->store(physical, std::memory_order_relaxed);
			completeFetch(node, physical, kPageSize);
			return true;
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
	size_t progress = 0;
	while(progress < length) {
		auto dest_misalign = (dest_offset + progress) % kPageSize;
		auto src_misalign = (src_offset + progress) % kPageSize;
		size_t chunk = frigg::min(frigg::min(kPageSize - dest_misalign,
				kPageSize - src_misalign), length - progress);

		PhysicalAddr dest_page = dest_memory->blockForRange(dest_offset + progress - dest_misalign);
		PhysicalAddr src_page = src_memory->blockForRange(src_offset + progress - dest_misalign);
		assert(dest_page != PhysicalAddr(-1));
		assert(src_page != PhysicalAddr(-1));

		PageAccessor dest_accessor{dest_page};
		PageAccessor src_accessor{src_page};
		memcpy((uint8_t *)dest_accessor.get() + dest_misalign,
				(uint8_t *)src_accessor.get() + src_misalign, chunk);

		progress += chunk;
	}
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

void Memory::submitInitiateLoad(InitiateBase *initiate) {
	switch(tag()) {
	case MemoryTag::frontal:
		static_cast<FrontalMemory *>(this)->submitInitiateLoad(initiate);
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
	size_t progress = 0;
	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, size);
		PhysicalAddr page = bundle->blockForRange(offset - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy((uint8_t *)accessor.get() + misalign, pointer, prefix);
		progress += prefix;
	}

	while(size - progress >= kPageSize) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = bundle->blockForRange(offset + progress);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy(accessor.get(), (uint8_t *)pointer + progress, kPageSize);
		progress += kPageSize;
	}

	if(size - progress > 0) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = bundle->blockForRange(offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{page};
		memcpy(accessor.get(), (uint8_t *)pointer + progress, size - progress);
	}

	complete(node);
}

void copyFromBundle(Memory *bundle, ptrdiff_t offset, void *buffer, size_t size,
		CopyFromBundleNode *node, void (*complete)(CopyFromBundleNode *)) {
	size_t progress = 0;
	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, size);
		PhysicalAddr page = bundle->blockForRange(offset - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy(buffer, (uint8_t *)accessor.get() + misalign, prefix);
		progress += prefix;
	}

	while(size - progress >= kPageSize) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = bundle->blockForRange(offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{page};
		memcpy((uint8_t *)buffer + progress, accessor.get(), kPageSize);
		progress += kPageSize;
	}

	if(size - progress > 0) {
		assert((offset + progress) % kPageSize == 0);
		PhysicalAddr page = bundle->blockForRange(offset + progress);
		assert(page != PhysicalAddr(-1));
		
		PageAccessor accessor{page};
		memcpy((uint8_t *)buffer + progress, accessor.get(), size - progress);
	}

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

PhysicalAddr HardwareMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	return _base + offset;
}

bool HardwareMemory::fetchRange(uintptr_t offset, FetchNode *node, void (*fetched)(FetchNode *)) {
	assert(offset % kPageSize == 0);
	setupFetch(node, fetched);

	completeFetch(node, _base + offset, _length - offset);
	return true;
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
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(!(new_length % _chunkSize));
	size_t num_chunks = new_length / _chunkSize;
	assert(num_chunks >= _physicalChunks.size());
	_physicalChunks.resize(num_chunks, PhysicalAddr(-1));
}

void AllocatedMemory::copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t size) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

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
			PageAccessor accessor{physical + pg_progress};
			memset(accessor.get(), 0, kPageSize);
		}
		_physicalChunks[index] = physical;
	}

	PageAccessor accessor{_physicalChunks[index]
			+ ((offset % _chunkSize) / kPageSize)};
	memcpy((uint8_t *)accessor.get() + (offset % kPageSize), pointer, size);
}

PhysicalAddr AllocatedMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	auto index = offset / _chunkSize;
	auto disp = offset & (_chunkSize - 1);
	assert(index < _physicalChunks.size());

	if(_physicalChunks[index] == PhysicalAddr(-1))
		return PhysicalAddr(-1);
	return _physicalChunks[index] + disp;
}

bool AllocatedMemory::fetchRange(uintptr_t offset, FetchNode *node, void (*fetched)(FetchNode *)) {
	setupFetch(node, fetched);
	
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	
	auto index = offset / _chunkSize;
	auto disp = offset & (_chunkSize - 1);
	assert(index < _physicalChunks.size());

	if(_physicalChunks[index] == PhysicalAddr(-1)) {
		auto physical = physicalAllocator->allocate(_chunkSize);
		assert(physical != PhysicalAddr(-1));
		assert(!(physical & (_chunkAlign - 1)));

		for(size_t pg_progress = 0; pg_progress < _chunkSize; pg_progress += kPageSize) {
			PageAccessor accessor{physical + pg_progress};
			memset(accessor.get(), 0, kPageSize);
		}
		_physicalChunks[index] = physical;
	}

	assert(_physicalChunks[index] != PhysicalAddr(-1));
	completeFetch(node, _physicalChunks[index] + disp, _chunkSize - disp);
	return true;
}

size_t AllocatedMemory::getLength() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

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

// TODO: Split this into a function to match initiate <-> handle requests
// + a different function to complete initiate requests.
void ManagedSpace::progressLoads() {
	// TODO: this function could issue loads > a single kPageSize
	while(!initiateLoadQueue.empty()) {
		auto initiate = initiateLoadQueue.front();

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
				initiateLoadQueue.pop_front();
			}else{
				initiateLoadQueue.pop_front();
				pendingLoadQueue.push_back(initiate);
			}
		}
	}
}

bool ManagedSpace::isComplete(InitiateBase *initiate) {
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

PhysicalAddr BackingMemory::peekRange(uintptr_t offset) {
	assert(!(offset % kPageSize));

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	return _managed->physicalPages[index];
}

bool BackingMemory::fetchRange(uintptr_t offset, FetchNode *node, void (*fetched)(FetchNode *)) {
	assert(!(offset % kPageSize));
	setupFetch(node, fetched);

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);
	
	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	if(_managed->physicalPages[index] == PhysicalAddr(-1)) {
		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		assert(physical != PhysicalAddr(-1));
		
		PageAccessor accessor{physical};
		memset(accessor.get(), 0, kPageSize);
		_managed->physicalPages[index] = physical;
	}

	completeFetch(node, _managed->physicalPages[index], kPageSize);
	return true;
}

size_t BackingMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->physicalPages.size() * kPageSize;
}

void BackingMemory::submitHandleLoad(frigg::SharedPtr<ManageBase> handle) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	_managed->handleLoadQueue.addBack(frigg::move(handle));
	_managed->progressLoads();
}

void BackingMemory::completeLoad(size_t offset, size_t length) {
	assert((offset % kPageSize) == 0);
	assert((length % kPageSize) == 0);
	
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);
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

	InitiateList queue;
	for(auto it = _managed->pendingLoadQueue.begin(); it != _managed->pendingLoadQueue.end(); ) {
		auto it_copy = it;
		auto node = *it++;
		if(_managed->isComplete(node)) {
			_managed->pendingLoadQueue.erase(it_copy);
			queue.push_back(node);
		}
	}

	irq_lock.unlock();
	lock.unlock();

	while(!queue.empty()) {
		auto node = queue.pop_front();
		node->complete(kErrSuccess);
	}
}

// --------------------------------------------------------
// FrontalMemory
// --------------------------------------------------------

PhysicalAddr FrontalMemory::peekRange(uintptr_t offset) {
	assert(!(offset % kPageSize));

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	if(_managed->loadState[index] != ManagedSpace::kStateLoaded)
		return PhysicalAddr(-1);
	return _managed->physicalPages[index];
}

bool FrontalMemory::fetchRange(uintptr_t offset, FetchNode *node, void (*fetched)(FetchNode *)) {
	assert(!(offset % kPageSize));
	setupFetch(node, fetched);
	
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	if(_managed->loadState[index] != ManagedSpace::kStateLoaded) {
		auto functor = [=] (Error error) {
			assert(error == kErrSuccess);

			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&_managed->mutex);

			auto physical = _managed->physicalPages[index];
			assert(physical != PhysicalAddr(-1));

			lock.unlock();
			irq_lock.unlock();

			completeFetch(node, physical, kPageSize);
			callbackFetch(node);
		};

		// TODO: Do not allocate memory here; use pre-allocated nodes instead.
		auto initiate = frigg::construct<Initiate<decltype(functor)>>(*kernelAlloc,
				offset, kPageSize, frigg::move(functor));
		_managed->initiateLoadQueue.push_back(initiate);
		_managed->progressLoads();
		return false;
	}

	auto physical = _managed->physicalPages[index];
	assert(physical != PhysicalAddr(-1));
	completeFetch(node, physical, kPageSize);
	return true;
}

size_t FrontalMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->physicalPages.size() * kPageSize;
}

void FrontalMemory::submitInitiateLoad(InitiateBase *initiate) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	assert(initiate->offset % kPageSize == 0);
	assert(initiate->length % kPageSize == 0);
	assert((initiate->offset + initiate->length) / kPageSize
			<= _managed->physicalPages.size());
	
	_managed->initiateLoadQueue.push_back(initiate);
	_managed->progressLoads();
}

// --------------------------------------------------------

ExteriorBundleView::ExteriorBundleView(frigg::SharedPtr<MemoryBundle> bundle,
		ptrdiff_t view_offset, size_t view_size)
: _bundle{frigg:move(bundle)}, _viewOffset{view_offset}, _viewSize{view_size} {
	assert(!(_viewOffset & (kPageSize - 1)));
	assert(!(_viewSize & (kPageSize - 1)));
}

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
		MappingFlags flags, frigg::SharedPtr<VirtualView> view, uintptr_t offset)
: Mapping{owner, address, length, flags}, _view{frigg::move(view)}, _offset{offset} { }

frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
NormalMapping::resolveRange(ptrdiff_t offset, size_t size) {
	assert(offset + size <= length());
	return _view->resolveRange(_offset + offset, frigg::min(size, length() - offset));
}

Mapping *NormalMapping::shareMapping(AddressSpace *dest_space) {
	// TODO: Always keep the exact flags?
	return frigg::construct<NormalMapping>(*kernelAlloc, dest_space,
			address(), length(), flags(), _view, _offset);
}

Mapping *NormalMapping::copyOnWrite(AddressSpace *dest_space) {
	auto chain = frigg::makeShared<CowBundle>(*kernelAlloc, _view, _offset, length());
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

		auto range = _view->resolveRange(_offset + progress, kPageSize);
		assert(range.get<2>() >= kPageSize);
		PhysicalAddr physical = range.get<0>()->peekRange(range.get<1>());

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

bool NormalMapping::handleFault(FaultNode *node) {
	if(node->_flags & AddressSpace::kFaultWrite)
		if(!((flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)) {
			node->_resolved = false;
			return true;
		}
	if(node->_flags & AddressSpace::kFaultExecute)
		if(!((flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)) {
			node->_resolved = false;
			return true;
		}

	auto fault_page = (node->_address - address()) & ~(kPageSize - 1);

	auto bundle_range = _view->resolveRange(_offset + fault_page, kPageSize);
	assert(bundle_range.get<2>() >= kPageSize);
	node->_bundleOffset = bundle_range.get<1>();

	static auto remap = [] (FaultNode *node) {
		auto self = node->_mapping;

		auto fault_page = (node->_address - self->address()) & ~(kPageSize - 1);
		auto vaddr = self->address() + fault_page;
		// TODO: This can actually happen!
		assert(!self->owner()->_pageSpace.isMapped(vaddr));

		uint32_t page_flags = 0;
		if((self->flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)
			page_flags |= page_access::write;
		if((self->flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)
			page_flags |= page_access::execute;
		// TODO: Allow inaccessible mappings.
		assert((self->flags() & MappingFlags::permissionMask) & MappingFlags::protRead);

		self->owner()->_pageSpace.mapSingle4k(vaddr, node->_fetch.range().get<0>(),
				true, page_flags);
		node->_resolved = true;
	};

	static auto fetched = [] (FetchNode *base) {
		auto node = frg::container_of(base, &FaultNode::_fetch);
		remap(node);
		node->_handled(node);
	};

	if(bundle_range.get<0>()->fetchRange(node->_bundleOffset, &node->_fetch, fetched)) {
		remap(node);
		return true;
	}else{
		return false;
	}
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

bool CowMapping::handleFault(FaultNode *node) {
	// TODO: We do not need to copy on read.
	if(node->_flags & AddressSpace::kFaultWrite)
		if(!((flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)) {
			node->_resolved = false;
			return false;
		}
	if(node->_flags & AddressSpace::kFaultExecute)
		if(!((flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)) {
			node->_resolved = false;
			return false;
		}

	uint32_t page_flags = 0;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)
		page_flags |= page_access::write;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)
		page_flags |= page_access::execute;
	// TODO: Allow inaccessible mappings.
	assert((flags() & MappingFlags::permissionMask) & MappingFlags::protRead);

	auto fault_page = (node->_address - address()) & ~(kPageSize - 1);

	auto physical = _cowBundle->blockForRange(fault_page);
	// TODO: Ensure that no racing threads still see the original page.
	owner()->_pageSpace.mapSingle4k(address() + fault_page, physical,
			true, page_flags);
	node->_resolved = true;
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
		frigg::UnsafePtr<VirtualView> view, VirtualAddr address,
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
			static_cast<MappingFlags>(mapping_flags), view.toShared(), offset);
	
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

bool AddressSpace::handleFault(VirtualAddr address, uint32_t fault_flags,
		FaultNode *node, void (*handled)(FaultNode *)) {
	node->_address = address;
	node->_flags = fault_flags;
	node->_handled = handled;

	Mapping *mapping;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&lock);

		mapping = _getMapping(address);
		if(!mapping)
			return false;
	}
	
	node->_mapping = mapping;
	
	// FIXME: mapping might be deleted here!
	// We need to use either refcounting or QS garbage collection here!

	return mapping->handleFault(node);
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
			if(false) {
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
				auto bundle = frigg::makeShared<AllocatedMemory>(*kernelAlloc,
						cur_mapping->length(), kPageSize, kPageSize);

				for(size_t pg = 0; pg < cur_mapping->length(); pg += kPageSize) {
					auto range = cur_mapping->resolveRange(pg, kPageSize);
					auto physical = range.get<0>()->blockForRange(range.get<1>());
					assert(physical != PhysicalAddr(-1));
					PageAccessor accessor{physical};
					bundle->copyKernelToThisSync(pg, accessor.get(), kPageSize);
				}
				
				auto view = frigg::makeShared<ExteriorBundleView>(*kernelAlloc,
						frigg::move(bundle), 0, cur_mapping->length());

				auto fork_mapping = frigg::construct<NormalMapping>(*kernelAlloc,
						fork_space.get(), cur_mapping->address(), cur_mapping->length(),
						cur_mapping->flags(), frigg::move(view), 0);
				fork_space->_mappings.insert(fork_mapping);
				fork_mapping->install(false);
			}
		}else{
			assert(!"Illegal mapping type");
		}

		cur_mapping = successor;
	}

	return frigg::move(fork_space);
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

// --------------------------------------------------------
// ForeignSpaceAccessor
// --------------------------------------------------------

bool ForeignSpaceAccessor::_processAcquire(AcquireNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&node->_accessor->_space->lock);

	while(node->_progress < node->_accessor->_length) {
		auto vaddr = reinterpret_cast<uintptr_t>(node->_accessor->_address) + node->_progress;
		auto mapping = node->_accessor->_space->_getMapping(vaddr);
		assert(mapping);
		auto range = mapping->resolveRange(vaddr - mapping->address(),
				node->_accessor->_length - node->_progress);
		if(!range.get<0>()->fetchRange(range.get<1>(), &node->_fetch, &_fetchedAcquire))
			return false;
		node->_progress += node->_fetch.range().get<1>();
	}

	return true;
}

void ForeignSpaceAccessor::_fetchedAcquire(FetchNode *base) {
	assert(!"This is untested");
	auto node = frg::container_of(base, &AcquireNode::_fetch);
	node->_progress += kPageSize;

	if(_processAcquire(node)) {
		node->_accessor->_acquired = true;
		node->_acquired(node);
	}
}

bool ForeignSpaceAccessor::acquire(AcquireNode *node, void (*acquired)(AcquireNode *)) {
	node->_acquired = acquired;
	node->_accessor = this;
	node->_progress = 0;

	if(_processAcquire(node)) {
		node->_accessor->_acquired = true;
		return true;
	}
	return false;
}

PhysicalAddr ForeignSpaceAccessor::getPhysical(size_t offset) {
	auto irq_lock = frigg::guard(&irqMutex());
	AddressSpace::Guard guard(&_space->lock);

	auto vaddr = reinterpret_cast<VirtualAddr>(_address) + offset;
	return _resolvePhysical(vaddr);
}

void ForeignSpaceAccessor::load(size_t offset, void *pointer, size_t size) {
	assert(_acquired);

	auto irq_lock = frigg::guard(&irqMutex());
	AddressSpace::Guard guard(&_space->lock);
	
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

Error ForeignSpaceAccessor::write(size_t offset, const void *pointer, size_t size) {
	assert(_acquired);

	auto irq_lock = frigg::guard(&irqMutex());
	AddressSpace::Guard guard(&_space->lock);
	
	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = (VirtualAddr)_address + offset + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = _resolvePhysical(write - misalign);
		if(page == PhysicalAddr(-1))
			return kErrFault;

		PageAccessor accessor{page};
		memcpy((char *)accessor.get() + misalign, (char *)pointer + progress, chunk);
		progress += chunk;
	}

	return kErrSuccess;
}

PhysicalAddr ForeignSpaceAccessor::_resolvePhysical(VirtualAddr vaddr) {
	Mapping *mapping = _space->_getMapping(vaddr);
	assert(mapping);
	auto range = mapping->resolveRange(vaddr - mapping->address(), kPageSize);
	auto physical = range.get<0>()->peekRange(range.get<1>());
	assert(physical != PhysicalAddr(-1));
	return physical;
}

} // namespace thor

