
#include <type_traits>
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
	constexpr bool logUncaching = false;

	void logRss(AddressSpace *space) {
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

// --------------------------------------------------------
// Reclaim implementation.
// --------------------------------------------------------

extern frigg::LazyInitializer<frigg::Vector<KernelFiber *, KernelAlloc>> earlyFibers;

struct MemoryReclaimer {
	void addPage(CachePage *page) {
		// TODO: Do we need the IRQ lock here?
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		assert(!(page->flags & CachePage::reclaimStateMask));
		_lruList.push_back(page);
		page->flags |= CachePage::reclaimCached;
		_cachedSize += kPageSize;
	}

	void bumpPage(CachePage *page) {
		// TODO: Do we need the IRQ lock here?
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		if((page->flags & CachePage::reclaimStateMask) == CachePage::reclaimCached) {
			auto it = _lruList.iterator_to(page);
			_lruList.erase(it);
		}else {
			assert((page->flags & CachePage::reclaimStateMask) == CachePage::reclaimUncaching);
			page->flags &= ~CachePage::reclaimStateMask;
			page->flags |= CachePage::reclaimCached;
			_cachedSize += kPageSize;
		}

		_lruList.push_back(page);
	}

	void removePage(CachePage *page) {
		// TODO: Do we need the IRQ lock here?
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		if((page->flags & CachePage::reclaimStateMask) == CachePage::reclaimCached) {
			auto it = _lruList.iterator_to(page);
			_lruList.erase(it);
			_cachedSize -= kPageSize;
		}else{
			assert((page->flags & CachePage::reclaimStateMask) == CachePage::reclaimUncaching);
		}
		page->flags &= ~CachePage::reclaimStateMask;
	}

	KernelFiber *createReclaimFiber() {
		auto checkReclaim = [this] () -> bool {
			// Take a single page out of the LRU list.
			// TODO: We have to acquire a refcount here.
			CachePage *page;
			{
				auto irq_lock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&_mutex);

				if(_cachedSize <= (1 << 20))
					return false;

				page = _lruList.pop_front();
				page->flags &= ~CachePage::reclaimStateMask;
				page->flags |= CachePage::reclaimUncaching;
				_cachedSize -= kPageSize;
			}

			// Evict the page and wait until it is evicted.
			struct Closure {
				FiberBlocker blocker;
				Worklet worklet;
				ReclaimNode node;
			} closure;

			closure.worklet.setup([] (Worklet *base) {
				auto closure = frg::container_of(base, &Closure::worklet);
				KernelFiber::unblockOther(&closure->blocker);
			});

			closure.blocker.setup();
			closure.node.setup(&closure.worklet);
			if(!page->bundle->uncachePage(page, &closure.node))
				KernelFiber::blockCurrent(&closure.blocker);

			return true;
		};

		return KernelFiber::post([=] {
			while(true) {
				if(logUncaching) {
					auto irq_lock = frigg::guard(&irqMutex());
					auto lock = frigg::guard(&_mutex);
					frigg::infoLogger() << "thor: " << (_cachedSize / 1024)
							<< " KiB of cached pages" << frigg::endLog;
				}

				while(checkReclaim())
					;
				fiberSleep(1'000'000'000);
			}
		});
	}

private:
	frigg::TicketLock _mutex;

	frg::intrusive_list<
		CachePage,
		frg::locate_member<
			CachePage,
			frg::default_list_hook<CachePage>,
			&CachePage::listHook
		>
	> _lruList;

	size_t _cachedSize = 0;
};

frigg::LazyInitializer<MemoryReclaimer> globalReclaimer;

void initializeReclaim() {
	globalReclaimer.initialize();
	earlyFibers->push(globalReclaimer->createReclaimFiber());
}

// --------------------------------------------------------
// MemoryView.
// --------------------------------------------------------

Error MemoryView::updateRange(ManageRequest type, size_t offset, size_t length) {
	return kErrIllegalObject;
}

// --------------------------------------------------------
// Memory
// --------------------------------------------------------

bool Memory::transfer(TransferNode *node) {
	node->_progress = 0;

	node->_srcBundle->lockRange(node->_srcOffset, node->_size);
	node->_destBundle->lockRange(node->_destOffset, node->_size);

	struct Ops {
		static bool process(TransferNode *node) {
			while(node->_progress < node->_size) {
				if(!prepareDestAndCopy(node))
					return false;
			}

			node->_srcBundle->unlockRange(node->_srcOffset, node->_size);
			node->_destBundle->unlockRange(node->_destOffset, node->_size);

			return true;
		}

		static bool prepareDestAndCopy(TransferNode *node) {
			auto dest_misalign = (node->_destOffset + node->_progress) % kPageSize;

			node->_worklet.setup(&Ops::fetchedDest);
			node->_destFetch.setup(&node->_worklet);
			if(!node->_destBundle->fetchRange(node->_destOffset + node->_progress - dest_misalign,
					&node->_destFetch))
				return false;
			return prepareSrcAndCopy(node);
		}

		static bool prepareSrcAndCopy(TransferNode *node) {
			auto src_misalign = (node->_srcOffset + node->_progress) % kPageSize;

			node->_worklet.setup(&Ops::fetchedSrc);
			node->_srcFetch.setup(&node->_worklet);
			if(!node->_srcBundle->fetchRange(node->_srcOffset + node->_progress - src_misalign,
					&node->_srcFetch))
				return false;
			return doCopy(node);
		}

		static bool doCopy(TransferNode *node) {
			auto dest_misalign = (node->_destOffset + node->_progress) % kPageSize;
			auto src_misalign = (node->_srcOffset + node->_progress) % kPageSize;
			size_t chunk = frigg::min(frigg::min(kPageSize - dest_misalign,
					kPageSize - src_misalign), node->_size - node->_progress);

			auto dest_page = node->_destFetch.range().get<0>();
			auto src_page = node->_srcFetch.range().get<0>();
			assert(dest_page != PhysicalAddr(-1));
			assert(src_page != PhysicalAddr(-1));

			PageAccessor dest_accessor{dest_page};
			PageAccessor src_accessor{src_page};
			memcpy((uint8_t *)dest_accessor.get() + dest_misalign,
					(uint8_t *)src_accessor.get() + src_misalign, chunk);

			node->_progress += chunk;
			return true;
		}

		static void fetchedDest(Worklet *base) {
			auto node = frg::container_of(base, &TransferNode::_worklet);
			if(!prepareSrcAndCopy(node))
				return;
			if(!process(node))
				return;

			WorkQueue::post(node->_copied);
		}

		static void fetchedSrc(Worklet *base) {
			auto node = frg::container_of(base, &TransferNode::_worklet);
			if(!doCopy(node))
				return;
			if(!process(node))
				return;

			WorkQueue::post(node->_copied);
		}
	};

	return Ops::process(node);
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

void Memory::submitInitiateLoad(MonitorNode *initiate) {
	switch(tag()) {
	case MemoryTag::frontal:
		static_cast<FrontalMemory *>(this)->submitInitiateLoad(initiate);
		break;
	case MemoryTag::hardware:
	case MemoryTag::allocated:
		initiate->setup(kErrSuccess);
		initiate->complete();
		break;
	case MemoryTag::copyOnWrite:
		assert(!"Not implemented yet");
	default:
		assert(!"Not supported");
	}
}

void Memory::submitManage(ManageNode *handle) {
	switch(tag()) {
	case MemoryTag::backing:
		static_cast<BackingMemory *>(this)->submitManage(handle);
		break;
	default:
		assert(!"Not supported");
	}
}

// --------------------------------------------------------
// Copy operations.
// --------------------------------------------------------

bool copyToBundle(MemoryView *view, ptrdiff_t offset, const void *pointer, size_t size,
		CopyToBundleNode *node, void (*complete)(CopyToBundleNode *)) {
	size_t progress = 0;
	size_t misalign = offset % kPageSize;

	view->lockRange(offset, size);

	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, size);

		node->_worklet.setup(nullptr);
		node->_fetch.setup(&node->_worklet);
		if(!view->fetchRange(offset - misalign, &node->_fetch))
			assert(!"Handle the asynchronous case");

		auto page = node->_fetch.range().get<0>();
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy((uint8_t *)accessor.get() + misalign, pointer, prefix);
		progress += prefix;
	}

	while(size - progress >= kPageSize) {
		assert(!((offset + progress) % kPageSize));

		node->_worklet.setup(nullptr);
		node->_fetch.setup(&node->_worklet);
		if(!view->fetchRange(offset + progress, &node->_fetch))
			assert(!"Handle the asynchronous case");

		auto page = node->_fetch.range().get<0>();
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy(accessor.get(), (uint8_t *)pointer + progress, kPageSize);
		progress += kPageSize;
	}

	if(size - progress > 0) {
		assert(!((offset + progress) % kPageSize));

		node->_worklet.setup(nullptr);
		node->_fetch.setup(&node->_worklet);
		if(!view->fetchRange(offset + progress, &node->_fetch))
			assert(!"Handle the asynchronous case");

		auto page = node->_fetch.range().get<0>();
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy(accessor.get(), (uint8_t *)pointer + progress, size - progress);
	}

	view->unlockRange(offset, size);
	return true;
}

bool copyFromBundle(MemoryView *view, ptrdiff_t offset, void *buffer, size_t size,
		CopyFromBundleNode *node, void (*complete)(CopyFromBundleNode *)) {
	struct Ops {
		static bool process(CopyFromBundleNode *node) {
			while(node->_progress < node->_size) {
				if(!fetchAndCopy(node))
					return false;
			}

			node->_view->unlockRange(node->_viewOffset, node->_size);
			return true;
		}

		static bool fetchAndCopy(CopyFromBundleNode *node) {
			// TODO: In principle, we do not need to call fetchRange() with page-aligned args.
			auto misalign = (node->_viewOffset + node->_progress) % kPageSize;

			node->_fetch.setup(&node->_worklet);
			node->_worklet.setup([] (Worklet *base) {
				auto node = frg::container_of(base, &CopyFromBundleNode::_worklet);
				doCopy(node);

				// Tail of synchronous path.
				if(!process(node))
					return;
				node->_complete(node);
			});
			if(!node->_view->fetchRange(node->_viewOffset + node->_progress - misalign,
					&node->_fetch))
				return false;
			doCopy(node);
			return true;
		}

		static void doCopy(CopyFromBundleNode *node) {
			// TODO: In principle, we do not need to call fetchRange() with page-aligned args.
			assert(node->_fetch.range().get<1>() >= kPageSize);
			auto misalign = (node->_viewOffset + node->_progress) % kPageSize;
			size_t chunk = frigg::min(kPageSize - misalign, node->_size - node->_progress);

			auto physical = node->_fetch.range().get<0>();
			assert(physical != PhysicalAddr(-1));
			PageAccessor accessor{physical};
			memcpy(reinterpret_cast<uint8_t *>(node->_buffer) + node->_progress,
					reinterpret_cast<uint8_t *>(accessor.get()) + misalign, chunk);
			node->_progress += chunk;
		}
	};

	node->_view = view;
	node->_viewOffset = offset;
	node->_buffer = buffer;
	node->_size = size;
	node->_complete = complete;

	node->_progress = 0;
	view->lockRange(offset, size);

	return Ops::process(node);
}

// --------------------------------------------------------
// HardwareMemory
// --------------------------------------------------------

HardwareMemory::HardwareMemory(PhysicalAddr base, size_t length, CachingMode cache_mode)
: Memory{MemoryTag::hardware}, _base{base}, _length{length}, _cacheMode{cache_mode} {
	assert(!(base % kPageSize));
	assert(!(length % kPageSize));
}

HardwareMemory::~HardwareMemory() {
	// For now we do nothing when deallocating hardware memory.
}

void HardwareMemory::addObserver(smarter::shared_ptr<MemoryObserver>) {
	// As we never evict memory, there is no need to handle observers.
}

void HardwareMemory::removeObserver(smarter::borrowed_ptr<MemoryObserver>) {
	// As we never evict memory, there is no need to handle observers.
}

void HardwareMemory::lockRange(uintptr_t offset, size_t size) {
	// Hardware memory is "always locked".
}

void HardwareMemory::unlockRange(uintptr_t offset, size_t size) {
	// Hardware memory is "always locked".
}

frigg::Tuple<PhysicalAddr, CachingMode> HardwareMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	return frigg::Tuple<PhysicalAddr, CachingMode>{_base + offset, _cacheMode};
}

bool HardwareMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	assert(offset % kPageSize == 0);

	completeFetch(node, _base + offset, _length - offset, _cacheMode);
	return true;
}

void HardwareMemory::markDirty(uintptr_t, size_t) {
	// We never evict memory, there is no need to track dirty pages.
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
	if(logUsage)
		frigg::infoLogger() << "thor: Releasing AllocatedMemory ("
				<< (physicalAllocator->numUsedPages() * 4) << " KiB in use)" << frigg::endLog;
	for(size_t i = 0; i < _physicalChunks.size(); ++i) {
		if(_physicalChunks[i] != PhysicalAddr(-1))
			physicalAllocator->free(_physicalChunks[i], _chunkSize);
	}
	if(logUsage)
		frigg::infoLogger() << "thor:     ("
				<< (physicalAllocator->numUsedPages() * 4) << " KiB in use)" << frigg::endLog;
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

void AllocatedMemory::addObserver(smarter::shared_ptr<MemoryObserver> observer) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
}

void AllocatedMemory::removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
}

void AllocatedMemory::lockRange(uintptr_t offset, size_t size) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
}

void AllocatedMemory::unlockRange(uintptr_t offset, size_t size) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
}

frigg::Tuple<PhysicalAddr, CachingMode> AllocatedMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	auto index = offset / _chunkSize;
	auto disp = offset & (_chunkSize - 1);
	assert(index < _physicalChunks.size());

	if(_physicalChunks[index] == PhysicalAddr(-1))
		return frigg::Tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
	return frigg::Tuple<PhysicalAddr, CachingMode>{_physicalChunks[index] + disp,
			CachingMode::null};
}

bool AllocatedMemory::fetchRange(uintptr_t offset, FetchNode *node) {
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
	completeFetch(node, _physicalChunks[index] + disp, _chunkSize - disp, CachingMode::null);
	return true;
}

void AllocatedMemory::markDirty(uintptr_t offset, size_t size) {
	// Do nothing for now.
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
: physicalPages(*kernelAlloc), loadState(*kernelAlloc), lockCount(*kernelAlloc) {
	assert(!(length & (kPageSize - 1)));
	physicalPages.resize(length >> kPageShift, PhysicalAddr(-1));
	loadState.resize(length >> kPageShift, kStateMissing);
	lockCount.resize(length >> kPageShift, 0);
	pages = frg::construct_n<CachePage>(*kernelAlloc, length >> kPageShift);
	for(size_t i = 0; i < (length >> kPageShift); i++)
		pages[i].bundle = this;
}

ManagedSpace::~ManagedSpace() {
	// TODO: Free all physical memory.
	// TODO: We also have to remove all Loaded/Evicting pages from the reclaimer.
	assert(!"Implement this");
}

bool ManagedSpace::uncachePage(CachePage *page, ReclaimNode *continuation) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex);

	if(!numObservers)
		return true;

	size_t index = page - pages;
	assert(loadState[index] == kStatePresent);
	loadState[index] = kStateEvicting;
	globalReclaimer->removePage(&pages[index]);

	struct Closure {
		ManagedSpace *bundle;
		size_t index;
		Worklet worklet;
		EvictNode node;
		ReclaimNode *continuation;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	static constexpr auto retirePage = [] (ManagedSpace *bundle, size_t index) {
		if(bundle->loadState[index] != kStateEvicting)
			return;
		assert(!bundle->lockCount[index]);

		if(logUncaching)
			frigg::infoLogger() << "\e[33mEvicting physical page\e[39m" << frigg::endLog;
		assert(bundle->physicalPages[index] != PhysicalAddr(-1));
		physicalAllocator->free(bundle->physicalPages[index], kPageSize);
		bundle->loadState[index] = kStateMissing;
		bundle->physicalPages[index] = PhysicalAddr(-1);
	};

	closure->worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		if(logUncaching)
			frigg::infoLogger() << "\e[33mEviction completes (slow-path)\e[39m" << frigg::endLog;
		{
			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&closure->bundle->mutex);

			retirePage(closure->bundle, closure->index);
		}

		closure->continuation->complete();
		frigg::destruct(*kernelAlloc, closure);
	});
	closure->bundle = this;
	closure->index = index;
	closure->continuation = continuation;

	if(logUncaching)
		frigg::infoLogger() << "\e[33mThere are " << numObservers
				<< " observers\e[39m" << frigg::endLog;
	closure->node.setup(&closure->worklet, numObservers);
	size_t fast_paths = 0;
	for(auto observer : observers)
		if(observer->observeEviction((page - pages) << kPageShift, kPageSize, &closure->node))
			fast_paths++;
	if(!fast_paths)
		return false;
	if(!closure->node.retirePending(fast_paths))
		return false;

	if(logUncaching)
		frigg::infoLogger() << "\e[33mEviction completes (fast-path)\e[39m" << frigg::endLog;
	retirePage(this, index);
	frigg::destruct(*kernelAlloc, closure);
	return true;
}

void ManagedSpace::retirePage(CachePage *page) {

}

// Note: Neither offset nor size are necessarily multiples of the page size.
void ManagedSpace::lockPages(uintptr_t offset, size_t size) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex);
	assert((offset + size) / kPageSize <= physicalPages.size());

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		size_t index = (offset + pg) / kPageSize;
		lockCount[index]++;
		if(lockCount[index] == 1) {
			if(loadState[index] == kStatePresent) {
				globalReclaimer->removePage(&pages[index]);
			}else if(loadState[index] == kStateEvicting) {
				// Stop the eviction to keep the page present.
				loadState[index] = kStatePresent;
			}
		}
		assert(loadState[index] != kStateEvicting);
	}
}

// Note: Neither offset nor size are necessarily multiples of the page size.
void ManagedSpace::unlockPages(uintptr_t offset, size_t size) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex);
	assert((offset + size) / kPageSize <= physicalPages.size());

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		size_t index = (offset + pg) / kPageSize;
		assert(lockCount[index] > 0);
		lockCount[index]--;
		if(!lockCount[index]) {
			if(loadState[index] == kStatePresent)
				globalReclaimer->addPage(&pages[index]);
		}
		assert(loadState[index] != kStateEvicting);
	}
}

void ManagedSpace::submitManagement(ManageNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex);

	_managementQueue.push_back(node);
	_progressManagement();
}

void ManagedSpace::submitMonitor(MonitorNode *node) {
	node->progress = 0;

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex);

	assert(node->offset % kPageSize == 0);
	assert(node->length % kPageSize == 0);
	assert((node->offset + node->length) / kPageSize
			<= physicalPages.size());

	_monitorQueue.push_back(node);
	_progressMonitors();
}

void ManagedSpace::_progressManagement() {
	// For now, we prefer writeback to initialization.
	// "Proper" priorization should probably be done in the userspace driver
	// (we do not want to store per-page priorities here).

	while(!_writebackList.empty() && !_managementQueue.empty()) {
		auto page = _writebackList.front();
		auto index = page - pages;

		// Fuse the request with adjacent pages in the list.
		ptrdiff_t count = 0;
		while(!_writebackList.empty()) {
			auto fuse_page = _writebackList.front();
			auto fuse_index = fuse_page - pages;
			if(fuse_index != index + count)
				break;
			assert(loadState[fuse_index] == kStateWantWriteback);
			loadState[fuse_index] = kStateWriteback;
			count++;
			_writebackList.pop_front();
		}
		assert(count);

		auto node = _managementQueue.pop_front();
		node->setup(kErrSuccess, ManageRequest::writeback,
				index << kPageShift, count << kPageShift);
		node->complete();
	}

	while(!_initializationList.empty() && !_managementQueue.empty()) {
		auto page = _initializationList.front();
		auto index = page - pages;

		// Fuse the request with adjacent pages in the list.
		ptrdiff_t count = 0;
		while(!_initializationList.empty()) {
			auto fuse_page = _initializationList.front();
			auto fuse_index = fuse_page - pages;
			if(fuse_index != index + count)
				break;
			assert(loadState[fuse_index] == kStateWantInitialization);
			loadState[fuse_index] = kStateInitialization;
			count++;
			_initializationList.pop_front();
		}
		assert(count);

		auto node = _managementQueue.pop_front();
		node->setup(kErrSuccess, ManageRequest::initialize,
				index << kPageShift, count << kPageShift);
		node->complete();
	}
}

void ManagedSpace::_progressMonitors() {
	// TODO: Accelerate this by storing the monitors in a RB tree ordered by their progress.
	auto progressNode = [&] (MonitorNode *node) -> bool {
		while(node->progress < node->length) {
			size_t index = (node->offset + node->progress) >> kPageShift;
			if(loadState[index] == kStateWantInitialization
					|| loadState[index] == kStateInitialization)
				return false;

			assert(loadState[index] == kStatePresent
					|| loadState[index] == kStateWantWriteback
					|| loadState[index] == kStateWriteback
					|| loadState[index] == kStateEvicting);
			node->progress += kPageSize;
		}
		return true;
	};

	for(auto it = _monitorQueue.begin(); it != _monitorQueue.end(); ) {
		auto it_copy = it;
		auto node = *it++;
		assert(node->type == ManageRequest::initialize);
		if(progressNode(node)) {
			_monitorQueue.erase(it_copy);
			node->setup(kErrSuccess);
			node->complete();
		}
	}
}

// --------------------------------------------------------
// BackingMemory
// --------------------------------------------------------

void BackingMemory::addObserver(smarter::shared_ptr<MemoryObserver> observer) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	_managed->observers.push_back(observer.get());
	_managed->numObservers++;
	observer.release(); // Reference is now owned by the ManagedSpace;
}

void BackingMemory::removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto it = _managed->observers.iterator_to(observer.get());
	_managed->observers.erase(it);
	_managed->numObservers--;
	observer.ctr()->decrement();
}

void BackingMemory::lockRange(uintptr_t offset, size_t size) {
	_managed->lockPages(offset, size);
}

void BackingMemory::unlockRange(uintptr_t offset, size_t size) {
	_managed->unlockPages(offset, size);
}

frigg::Tuple<PhysicalAddr, CachingMode> BackingMemory::peekRange(uintptr_t offset) {
	assert(!(offset % kPageSize));

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	return frigg::Tuple<PhysicalAddr, CachingMode>{_managed->physicalPages[index],
			CachingMode::null};
}

bool BackingMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);
	assert(index < _managed->physicalPages.size());
	if(_managed->physicalPages[index] == PhysicalAddr(-1)) {
		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		assert(physical != PhysicalAddr(-1));

		PageAccessor accessor{physical};
		memset(accessor.get(), 0, kPageSize);
		_managed->physicalPages[index] = physical;
	}

	completeFetch(node, _managed->physicalPages[index] + misalign, kPageSize - misalign,
			CachingMode::null);
	return true;
}

void BackingMemory::markDirty(uintptr_t offset, size_t size) {
	// Writes through the BackingMemory do not affect the dirty state!
}

size_t BackingMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->physicalPages.size() * kPageSize;
}

void BackingMemory::submitManage(ManageNode *node) {
	_managed->submitManagement(node);
}

Error BackingMemory::updateRange(ManageRequest type, size_t offset, size_t length) {
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

	if(type == ManageRequest::initialize) {
		for(size_t pg = 0; pg < length; pg += kPageSize) {
			size_t index = (offset + pg) / kPageSize;
			assert(_managed->loadState[index] == ManagedSpace::kStateInitialization);
			_managed->loadState[index] = ManagedSpace::kStatePresent;
			if(!_managed->lockCount[index])
				globalReclaimer->addPage(&_managed->pages[index]);
		}
	}else{
		for(size_t pg = 0; pg < length; pg += kPageSize) {
			size_t index = (offset + pg) / kPageSize;
			// TODO: Handle kStateAnotherWriteback here by entering kStateWantWriteback again
			//       and by re-inserting the page into the _writebackList.
			assert(_managed->loadState[index] == ManagedSpace::kStateWriteback);
			_managed->loadState[index] = ManagedSpace::kStatePresent;
			if(!_managed->lockCount[index])
				globalReclaimer->addPage(&_managed->pages[index]);
		}
	}

	_managed->_progressMonitors();

	return kErrSuccess;
}

// --------------------------------------------------------
// FrontalMemory
// --------------------------------------------------------

void FrontalMemory::addObserver(smarter::shared_ptr<MemoryObserver> observer) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	_managed->observers.push_back(observer.get());
	_managed->numObservers++;
	observer.release(); // Reference is now owned by the ManagedSpace;
}

void FrontalMemory::removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto it = _managed->observers.iterator_to(observer.get());
	_managed->observers.erase(it);
	_managed->numObservers--;
	observer.ctr()->decrement();
}

void FrontalMemory::lockRange(uintptr_t offset, size_t size) {
	_managed->lockPages(offset, size);
}

void FrontalMemory::unlockRange(uintptr_t offset, size_t size) {
	_managed->unlockPages(offset, size);
}

frigg::Tuple<PhysicalAddr, CachingMode> FrontalMemory::peekRange(uintptr_t offset) {
	assert(!(offset % kPageSize));

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->physicalPages.size());
	if(_managed->loadState[index] != ManagedSpace::kStatePresent)
		return frigg::Tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
	return frigg::Tuple<PhysicalAddr, CachingMode>{_managed->physicalPages[index],
			CachingMode::null};
}

bool FrontalMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);
	assert(index < _managed->physicalPages.size());

	// Try the fast-paths first.
	if(_managed->loadState[index] == ManagedSpace::kStatePresent
			|| _managed->loadState[index] == ManagedSpace::kStateWantWriteback
			|| _managed->loadState[index] == ManagedSpace::kStateWriteback
			|| _managed->loadState[index] == ManagedSpace::kStateAnotherWriteback
			|| _managed->loadState[index] == ManagedSpace::kStateEvicting) {
		auto physical = _managed->physicalPages[index];
		assert(physical != PhysicalAddr(-1));

		if(_managed->loadState[index] == ManagedSpace::kStatePresent) {
			if(!_managed->lockCount[index])
				globalReclaimer->bumpPage(&_managed->pages[index]);
		}else if(_managed->loadState[index] == ManagedSpace::kStateEvicting) {
			// Cancel evication -- the page is still needed.
			_managed->loadState[index] = ManagedSpace::kStatePresent;
			globalReclaimer->addPage(&_managed->pages[index]);
		}

		completeFetch(node, physical + misalign, kPageSize - misalign, CachingMode::null);
		return true;
	}else{
		assert(_managed->loadState[index] == ManagedSpace::kStateMissing
				|| _managed->loadState[index] == ManagedSpace::kStateWantInitialization
				|| _managed->loadState[index] == ManagedSpace::kStateInitialization);
	}

	assert(!(node->flags() & FetchNode::disallowBacking));

	// We have to take the slow-path, i.e., perform the fetch asynchronously.
	if(_managed->loadState[index] == ManagedSpace::kStateMissing) {
		_managed->loadState[index] = ManagedSpace::kStateWantInitialization;
		_managed->_initializationList.push_back(&_managed->pages[index]);
	}
	_managed->_progressManagement();

	// TODO: Do not allocate memory here; use pre-allocated nodes instead.
	struct Closure {
		uintptr_t offset;
		FetchNode *fetch;
		ManagedSpace *bundle;

		Worklet worklet;
		MonitorNode initiate;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static void initiated(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &Closure::worklet);
			assert(closure->initiate.error() == kErrSuccess);

			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&closure->bundle->mutex);

			auto index = closure->offset >> kPageShift;
			auto misalign = closure->offset & (kPageSize - 1);
			assert(closure->bundle->loadState[index] == ManagedSpace::kStatePresent);
			auto physical = closure->bundle->physicalPages[index];
			assert(physical != PhysicalAddr(-1));

			lock.unlock();
			irq_lock.unlock();

			completeFetch(closure->fetch, physical + misalign, kPageSize - misalign,
					CachingMode::null);
			callbackFetch(closure->fetch);
			frigg::destruct(*kernelAlloc, closure);
		}
	};

	closure->offset = offset;
	closure->fetch = node;
	closure->bundle = _managed.get();

	closure->worklet.setup(&Ops::initiated);
	closure->initiate.setup(ManageRequest::initialize,
			offset, kPageSize, &closure->worklet);
	closure->initiate.progress = 0;
	_managed->_monitorQueue.push_back(&closure->initiate);
	_managed->_progressMonitors();

	return false;
}

void FrontalMemory::markDirty(uintptr_t offset, size_t size) {
	assert(!(offset % kPageSize));
	assert(!(size % kPageSize));

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	// Put the pages into the dirty state.
	assert(size == kPageSize);
	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto index = (offset + pg) >> kPageShift;
		if(_managed->loadState[index] == ManagedSpace::kStatePresent) {
			_managed->loadState[index] = ManagedSpace::kStateWantWriteback;
			if(!_managed->lockCount[index])
				globalReclaimer->removePage(&_managed->pages[index]);
			_managed->_writebackList.push_back(&_managed->pages[index]);
		}else if(_managed->loadState[index] == ManagedSpace::kStateWriteback) {
			// TODO: Implement this by entering kStateAnotherWriteback.
			assert(!"Re-scheduling of writebacks is not implemented");
		}else{
			assert(_managed->loadState[index] == ManagedSpace::kStateWantWriteback
					|| _managed->loadState[index] == ManagedSpace::kStateAnotherWriteback);
			return;
		}
	}

	_managed->_progressManagement();
}

size_t FrontalMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->physicalPages.size() * kPageSize;
}

void FrontalMemory::submitInitiateLoad(MonitorNode *node) {
	// TODO: This assumes that we want to load the range (which might not be true).
	assert(node->offset % kPageSize == 0);
	assert(node->length % kPageSize == 0);
	for(size_t pg = 0; pg < node->length; pg += kPageSize) {
		auto index = (node->offset + pg) >> kPageShift;
		if(_managed->loadState[index] == ManagedSpace::kStateMissing) {
			_managed->loadState[index] = ManagedSpace::kStateWantInitialization;
			_managed->_initializationList.push_back(&_managed->pages[index]);
		}
	}
	_managed->_progressManagement();

	_managed->submitMonitor(node);
}

// --------------------------------------------------------

MemorySlice::MemorySlice(frigg::SharedPtr<MemoryView> view,
		ptrdiff_t view_offset, size_t view_size)
: _view{frigg:move(view)}, _viewOffset{view_offset}, _viewSize{view_size} {
	assert(!(_viewOffset & (kPageSize - 1)));
	assert(!(_viewSize & (kPageSize - 1)));
}

size_t MemorySlice::length() {
	return _viewSize;
}

SliceRange MemorySlice::translateRange(ptrdiff_t offset, size_t size) {
	assert(offset + size <= _viewSize);
	return SliceRange{_view.get(), _viewOffset + offset,
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

Mapping::Mapping(smarter::shared_ptr<AddressSpace> owner, VirtualAddr base_address, size_t length,
		MappingFlags flags)
: _owner{std::move(owner)}, _address{base_address}, _length{length}, _flags{flags} { }

bool Mapping::populateVirtualRange(PopulateVirtualNode *continuation) {
	struct Closure {
		Mapping *self;
		PopulateVirtualNode *continuation;
		size_t progress = 0;
		Worklet worklet;
		TouchVirtualNode touchNode;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static bool process(Closure *closure) {
			while(closure->progress < closure->continuation->_size)
				if(!doTouch(closure))
					return false;
			return true;
		}

		static bool doTouch(Closure *closure) {
			auto self = closure->self;

			closure->touchNode.setup(closure->continuation->_offset + closure->progress,
					&closure->worklet);
			closure->worklet.setup([] (Worklet *base) {
				auto closure = frg::container_of(base, &Closure::worklet);
				closure->progress += closure->touchNode.range().get<1>();
				if(!process(closure))
					return;
				WorkQueue::post(closure->continuation->_prepared);
				frigg::destruct(*kernelAlloc, closure);
			});
			if(!self->touchVirtualPage(&closure->touchNode))
				return false;
			closure->progress += closure->touchNode.range().get<1>();
			return true;
		}

	};

	closure->self = this;
	closure->continuation = continuation;

	if(!Ops::process(closure))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	return true;
}

// --------------------------------------------------------
// NormalMapping
// --------------------------------------------------------

NormalMapping::NormalMapping(smarter::shared_ptr<AddressSpace> owner,
		VirtualAddr address, size_t length,
		MappingFlags flags, frigg::SharedPtr<MemorySlice> view, uintptr_t offset)
: Mapping{std::move(owner), address, length, flags},
		_slice{frigg::move(view)}, _viewOffset{offset} {
	assert(_viewOffset + NormalMapping::length() <= _slice->length());
	_view = _slice->getView();
}

NormalMapping::~NormalMapping() {
	assert(_state == MappingState::retired);
	//frigg::infoLogger() << "\e[31mthor: NormalMapping is destructed\e[39m" << frigg::endLog;
}

bool NormalMapping::lockVirtualRange(LockVirtualNode *node) {
	_view->lockRange(_viewOffset + node->offset(), node->size());
	return true;
}

void NormalMapping::unlockVirtualRange(uintptr_t offset, size_t size) {
	_view->unlockRange(_viewOffset + offset, size);
}

frigg::Tuple<PhysicalAddr, CachingMode>
NormalMapping::resolveRange(ptrdiff_t offset) {
	assert(_state == MappingState::active);

	// TODO: This function should be rewritten.
	assert((size_t)offset + kPageSize <= length());
	auto range = _slice->translateRange(_viewOffset + offset,
			frigg::min((size_t)kPageSize, length() - (size_t)offset));
	auto bundle_range = range.view->peekRange(range.displacement);
	return frigg::Tuple<PhysicalAddr, CachingMode>{bundle_range.get<0>(), bundle_range.get<1>()};
}

bool NormalMapping::touchVirtualPage(TouchVirtualNode *continuation) {
	assert(_state == MappingState::active);

	struct Closure {
		NormalMapping *self;
		FetchNode fetch;
		Worklet worklet;
		TouchVirtualNode *continuation;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static bool doFetch(Closure *closure) {
			auto self = closure->self;

			FetchFlags fetch_flags = 0;
			if(self->flags() & MappingFlags::dontRequireBacking)
				fetch_flags |= FetchNode::disallowBacking;

			closure->fetch.setup(&closure->worklet, fetch_flags);
			closure->worklet.setup([] (Worklet *base) {
				auto closure = frg::container_of(base, &Closure::worklet);
				closure->continuation->setResult(closure->fetch.range());
				WorkQueue::post(closure->continuation->_worklet);
				frigg::destruct(*kernelAlloc, closure);
			});
			if(!self->_view->fetchRange(self->_viewOffset + closure->continuation->_offset,
					&closure->fetch))
				return false;

			return true;
		}
	};

	closure->self = this;
	closure->continuation = continuation;

	if(!Ops::doFetch(closure))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	closure->continuation->setResult(closure->fetch.range());
	return true;
}

Mapping *NormalMapping::shareMapping(smarter::shared_ptr<AddressSpace> dest_space) {
	// TODO: Always keep the exact flags?
	auto ptr = smarter::allocate_shared<NormalMapping>(Allocator{}, std::move(dest_space),
			address(), length(), flags(), _slice, _viewOffset);
	ptr->selfPtr = ptr;
	auto mapping = ptr.get();
	ptr.release(); // AddressSpace owns one reference.
	return mapping;
}

Mapping *NormalMapping::copyOnWrite(smarter::shared_ptr<AddressSpace> dest_space) {
	assert(!"Fix construction of Mapping");
	__builtin_trap();
//	auto chain = frigg::makeShared<CowChain>(*kernelAlloc, _slice, _viewOffset, length());
//	return frigg::construct<CowMapping>(*kernelAlloc, dest_space,
//			address(), length(), flags(), frigg::move(chain));
}

void NormalMapping::install(bool overwrite) {
	assert(_state == MappingState::null);
	_state = MappingState::active;
	_view->addObserver(smarter::static_pointer_cast<NormalMapping>(selfPtr.lock()));

	uint32_t page_flags = 0;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)
		page_flags |= page_access::write;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)
		page_flags |= page_access::execute;
	// TODO: Allow inaccessible mappings.
	assert((flags() & MappingFlags::permissionMask) & MappingFlags::protRead);

	for(size_t progress = 0; progress < length(); progress += kPageSize) {
		auto range = _slice->translateRange(_viewOffset + progress, kPageSize);
		assert(range.size >= kPageSize);
		auto bundle_range = range.view->peekRange(range.displacement);

		VirtualAddr vaddr = address() + progress;
		if(overwrite && owner()->_pageSpace.isMapped(vaddr)) {
			owner()->_pageSpace.unmapRange(vaddr, kPageSize, PageMode::normal);
			owner()->_residuentSize -= kPageSize;
		}else{
			assert(!owner()->_pageSpace.isMapped(vaddr));
		}
		if(bundle_range.get<0>() != PhysicalAddr(-1)) {
			owner()->_pageSpace.mapSingle4k(vaddr, bundle_range.get<0>(), true,
					page_flags, bundle_range.get<1>());
			owner()->_residuentSize += kPageSize;
			logRss(owner());
		}
	}
}

void NormalMapping::uninstall(bool clear) {
	assert(_state == MappingState::active);
	_state = MappingState::zombie;

	if(clear) {
		for(size_t pg = 0; pg < length(); pg += kPageSize) {
			auto status = owner()->_pageSpace.unmapSingle4k(address() + pg);
			if(!(status & page_status::present))
				continue;
			if(status & page_status::dirty)
				_view->markDirty(_viewOffset + pg, kPageSize);
			owner()->_residuentSize -= kPageSize;
		}
	}
}

void NormalMapping::retire() {
	assert(_state == MappingState::zombie);
	_view->removeObserver(smarter::static_pointer_cast<NormalMapping>(selfPtr));
	_state = MappingState::retired;
}

bool NormalMapping::observeEviction(uintptr_t evict_offset, size_t evict_length,
		EvictNode *continuation) {
	assert(_state == MappingState::active);

	if(evict_offset + evict_length <= _viewOffset
			|| evict_offset >= _viewOffset + length())
		return true;

	// Begin and end offsets of the region that we need to unmap.
	auto shoot_begin = frg::max(evict_offset, _viewOffset);
	auto shoot_end = frg::min(evict_offset + evict_length, _viewOffset + length());

	// Offset from the beginning of the mapping.
	auto shoot_offset = shoot_begin - _viewOffset;
	auto shoot_size = shoot_end - shoot_begin;
	assert(shoot_size);
	assert(!(shoot_offset & (kPageSize - 1)));
	assert(!(shoot_size & (kPageSize - 1)));

	// TODO: Perform proper locking here!

	// Unmap the memory range.
	for(size_t pg = 0; pg < shoot_size; pg += kPageSize) {
		auto status = owner()->_pageSpace.unmapSingle4k(address() + shoot_offset + pg);
		if(!(status & page_status::present))
			continue;
		if(status & page_status::dirty)
			_view->markDirty(_viewOffset + shoot_offset + pg, kPageSize);
		owner()->_residuentSize -= kPageSize;
	}

	// Perform shootdown.
	struct Closure {
		smarter::shared_ptr<Mapping> mapping; // Need to keep the Mapping alive.
		Worklet worklet;
		ShootNode node;
		EvictNode *continuation;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	closure->worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		closure->continuation->done();
		frigg::destruct(*kernelAlloc, closure);
	});
	closure->mapping = selfPtr.lock();
	closure->continuation = continuation;

	closure->node.address = address() + shoot_offset;
	closure->node.size = shoot_size;
	closure->node.setup(&closure->worklet);
	if(!owner()->_pageSpace.submitShootdown(&closure->node))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	return true;
}

// --------------------------------------------------------
// CowMapping
// --------------------------------------------------------

CowChain::CowChain(frigg::SharedPtr<MemorySlice> view, ptrdiff_t offset, size_t size)
: _superRoot{frigg::move(view)}, _superOffset{offset}, _pages{kernelAlloc.get()} {
	assert(!(size & (kPageSize - 1)));
	_copy = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, kPageSize, kPageSize);
}

CowChain::CowChain(frigg::SharedPtr<CowChain> chain, ptrdiff_t offset, size_t size)
: _superChain{frigg::move(chain)}, _superOffset{offset}, _pages{kernelAlloc.get()} {
	assert(!(size & (kPageSize - 1)));
	_copy = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, kPageSize, kPageSize);
}

CowChain::~CowChain() {
	if(logUsage)
		frigg::infoLogger() << "thor: Releasing CowChain" << frigg::endLog;
}

// --------------------------------------------------------

CowMapping::CowMapping(smarter::shared_ptr<AddressSpace> owner,
		VirtualAddr address, size_t length,
		MappingFlags flags, frigg::SharedPtr<CowChain> chain)
: Mapping{std::move(owner), address, length, flags}, _chain{frigg::move(chain)},
		_lockCount{*kernelAlloc} {
	_lockCount.resize(length >> kPageShift, 0);
}

CowMapping::~CowMapping() {
	assert(_state == MappingState::retired);
	//frigg::infoLogger() << "\e[31mthor: CowMapping is destructed\e[39m" << frigg::endLog;
}

bool CowMapping::lockVirtualRange(LockVirtualNode *continuation) {
	assert(_state == MappingState::active);
	// For now, it is enough to populate the range, as pages can only be evicted from
	// the root of the CoW chain, but copies are never evicted.

	for(size_t pg = 0; pg < continuation->size(); pg += kPageSize) {
		auto index = (continuation->offset() + pg) >> kPageShift;
		_lockCount[index]++;
	}

	struct Closure {
		Worklet worklet;
		PopulateVirtualNode populateVirtual;
		LockVirtualNode *continuation;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	closure->continuation = continuation;

	closure->populateVirtual.setup(continuation->offset(), continuation->size(),
			&closure->worklet);
	closure->worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		LockVirtualNode::post(closure->continuation);
		frigg::destruct(*kernelAlloc, closure);
	});
	if(!populateVirtualRange(&closure->populateVirtual))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	return true;
}

void CowMapping::unlockVirtualRange(uintptr_t offset, size_t size) {
	assert(_state == MappingState::active);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto index = (offset + pg) >> kPageShift;
		assert(_lockCount[index] > 0);
		_lockCount[index]--;
	}
}

frigg::Tuple<PhysicalAddr, CachingMode>
CowMapping::resolveRange(ptrdiff_t offset) {
	assert(_state == MappingState::active);

	if(auto it = _chain->_pages.find(offset >> kPageShift); it) {
		auto physical = it->load(std::memory_order_relaxed);
		return frigg::Tuple<PhysicalAddr, CachingMode>{physical, CachingMode::null};
	}

	return frigg::Tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
}

bool CowMapping::touchVirtualPage(TouchVirtualNode *continuation) {
	assert(_state == MappingState::active);

	struct Closure {
		CowMapping *self;
		TransferNode copy;
		Worklet worklet;
		TouchVirtualNode *continuation;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static bool copyPage(Closure *closure) {
			auto self = closure->self;
			auto page_offset = closure->continuation->_offset & ~(kPageSize - 1);
			auto misalign = closure->continuation->_offset & (kPageSize - 1);

			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&self->_chain->_mutex);

			// If the page is present in this bundle we just return it.
			if(auto it = self->_chain->_pages.find(page_offset >> kPageShift); it) {
				auto physical = it->load(std::memory_order_relaxed);
				assert(physical != PhysicalAddr(-1));
				closure->continuation->setResult(physical, kPageSize - misalign,
						CachingMode::null);
				return true;
			}

			// Otherwise we need to copy from the super-tree.
			auto source = self->_chain.get();
			ptrdiff_t chain_disp = 0;
			while(true) {
				// Copy from a descendant CoW bundle.
				if(auto it = source->_pages.find((chain_disp + page_offset) >> kPageShift); it) {
					// Cannot copy from ourselves; this case is handled above.
					assert(source != self->_chain.get());
					closure->copy.setup(self->_chain->_copy.get(), page_offset,
							source->_copy.get(),
							chain_disp + page_offset, kPageSize,
							nullptr);
					if(!Memory::transfer(&closure->copy))
						assert(!"Fix the asynchronous case");

					auto range = self->_chain->_copy->peekRange(page_offset);
					auto physical = range.get<0>();
					assert(physical != PhysicalAddr(-1));
					closure->continuation->setResult(physical, kPageSize - misalign,
							range.get<1>());
					auto cow_it = self->_chain->_pages.insert(page_offset >> kPageShift,
							PhysicalAddr(-1));
					cow_it->store(physical, std::memory_order_relaxed);
					return true;
				}

				// Copy from the root view.
				if(!source->_superChain) {
					assert(source->_superRoot);

					closure->worklet.setup([] (Worklet *base) {
						auto closure = frg::container_of(base, &Closure::worklet);
						auto self = closure->self;

						auto irq_lock = frigg::guard(&irqMutex());
						auto lock = frigg::guard(&self->_chain->_mutex);

						finishPage(closure);

						WorkQueue::post(closure->continuation->_worklet);
						frigg::destruct(*kernelAlloc, closure);
					});
					closure->copy.setup(self->_chain->_copy.get(), page_offset,
							source->_superRoot->getView().get(),
							source->_superOffset + chain_disp + page_offset, kPageSize,
							&closure->worklet);
					if(!Memory::transfer(&closure->copy))
						return false;

					finishPage(closure);
					return true;
				}

				chain_disp += source->_superOffset;
				source = source->_superChain.get();
			}
		}

		static void finishPage(Closure *closure) {
			// TODO: Assert that there is no overflow.
			auto self = closure->self;
			auto page_offset = closure->continuation->_offset & ~(kPageSize - 1);
			auto misalign = closure->continuation->_offset & (kPageSize - 1);

			auto range = self->_chain->_copy->peekRange(page_offset);
			auto physical = range.get<0>();
			assert(physical != PhysicalAddr(-1));
			closure->continuation->setResult(physical, kPageSize - misalign, range.get<1>());
			auto cow_it = self->_chain->_pages.insert(page_offset >> kPageShift,
					PhysicalAddr(-1));
			cow_it->store(physical, std::memory_order_relaxed);
		}
	};

	closure->self = this;
	closure->continuation = continuation;

	if(!Ops::copyPage(closure))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	return true;
}

Mapping *CowMapping::shareMapping(smarter::shared_ptr<AddressSpace> dest_space) {
	(void)dest_space;
	assert(!"Fix this");
	__builtin_unreachable();
}

Mapping *CowMapping::copyOnWrite(smarter::shared_ptr<AddressSpace> dest_space) {
	assert(!"Fix construction of Mapping");
	__builtin_trap();
//	auto sub_chain = frigg::makeShared<CowChain>(*kernelAlloc, _chain, 0, length());
//	return frigg::construct<CowMapping>(*kernelAlloc, dest_space,
//			address(), length(), flags(), frigg::move(sub_chain));
}

void CowMapping::install(bool overwrite) {
	assert(_state == MappingState::null);
	_state = MappingState::active;

	assert(_chain->_superRoot && "TODO: fix eviction of chains");
	_chain->_superRoot->getView()->addObserver(
			smarter::static_pointer_cast<CowMapping>(selfPtr.lock()));

	// For now we just unmap everything. TODO: Map available pages.
	for(size_t progress = 0; progress < length(); progress += kPageSize) {
		VirtualAddr vaddr = address() + progress;
		if(overwrite && owner()->_pageSpace.isMapped(vaddr)) {
			owner()->_pageSpace.unmapRange(vaddr, kPageSize, PageMode::normal);
			owner()->_residuentSize -= kPageSize;
		}else{
			assert(!owner()->_pageSpace.isMapped(vaddr));
		}
	}
}

void CowMapping::uninstall(bool clear) {
	assert(_state == MappingState::active);
	_state = MappingState::zombie;

	assert(_chain->_superRoot && "TODO: fix eviction of chains");

	if(clear) {
		for(size_t pg = 0; pg < length(); pg += kPageSize)
			if(owner()->_pageSpace.isMapped(address() + pg))
				owner()->_residuentSize -= kPageSize;
		owner()->_pageSpace.unmapRange(address(), length(), PageMode::remap);
	}
}

void CowMapping::retire() {
	assert(_state == MappingState::zombie);
	_chain->_superRoot->getView()->removeObserver(
			smarter::static_pointer_cast<CowMapping>(selfPtr));
	_state = MappingState::retired;
}

bool CowMapping::observeEviction(uintptr_t evict_offset, size_t evict_length,
		EvictNode *continuation) {
	assert(_state == MappingState::active);
	assert(_chain->_superRoot && "TODO: fix eviction of chains");

	if(evict_offset + evict_length <= _chain->_superOffset
			|| evict_offset >= _chain->_superOffset + length())
		return true;

	// Begin and end offsets of the region that we need to unmap.
	auto shoot_begin = frg::max(evict_offset, static_cast<size_t>(_chain->_superOffset));
	auto shoot_end = frg::min(evict_offset + evict_length, _chain->_superOffset + length());

	// Offset from the beginning of the mapping.
	auto shoot_offset = shoot_begin - _chain->_superOffset;
	auto shoot_size = shoot_end - shoot_begin;
	assert(shoot_size);
	assert(!(shoot_offset & (kPageSize - 1)));
	assert(!(shoot_size & (kPageSize - 1)));

	// TODO: Perform proper locking here!

	// Unmap the memory range.
	// TODO: This is only necessary if the page was not copied already.
	for(size_t pg = 0; pg < shoot_size; pg += kPageSize)
		if(owner()->_pageSpace.isMapped(address() + shoot_offset + pg))
			owner()->_residuentSize -= kPageSize;
	owner()->_pageSpace.unmapRange(address() + shoot_offset, shoot_size, PageMode::remap);

	// Perform shootdown.
	struct Closure {
		smarter::shared_ptr<Mapping> mapping; // Need to keep the Mapping alive.
		Worklet worklet;
		ShootNode node;
		EvictNode *continuation;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	closure->worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		closure->continuation->done();
		frigg::destruct(*kernelAlloc, closure);
	});
	closure->mapping = selfPtr.lock();
	closure->continuation = continuation;

	closure->node.address = address() + shoot_offset;
	closure->node.size = shoot_size;
	closure->node.setup(&closure->worklet);
	if(!owner()->_pageSpace.submitShootdown(&closure->node))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	return true;
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

void AddressSpace::activate(smarter::shared_ptr<AddressSpace, BindableHandle> space) {
	auto page_space = &space->_pageSpace;
	PageSpace::activate(smarter::shared_ptr<PageSpace>{space->selfPtr.lock(), page_space});
}

AddressSpace::AddressSpace() { }

AddressSpace::~AddressSpace() {
	if(logCleanup)
		frigg::infoLogger() << "\e[31mthor: AddressSpace is destructed\e[39m" << frigg::endLog;
}

void AddressSpace::dispose(BindableHandle) {
	if(logCleanup)
		frigg::infoLogger() << "\e[31mthor: AddressSpace is cleared\e[39m" << frigg::endLog;

	while(_holes.get_root()) {
		auto hole = _holes.get_root();
		_holes.remove(hole);
		frg::destruct(*kernelAlloc, hole);
	}

	while(_mappings.get_root()) {
		auto mapping = _mappings.get_root();
		mapping->uninstall(true);
		mapping->retire(); // TODO: We have to shootdown first.
		_mappings.remove(mapping);
		mapping->selfPtr.ctr()->decrement();
	}

	_pageSpace.retire();
}

void AddressSpace::setupDefaultMappings() {
	auto hole = frigg::construct<Hole>(*kernelAlloc, 0x100000, 0x7ffffff00000);
	_holes.insert(hole);
}

Error AddressSpace::map(Guard &guard,
		frigg::UnsafePtr<MemorySlice> view, VirtualAddr address,
		size_t offset, size_t length, uint32_t flags, VirtualAddr *actual_address) {
	assert(guard.protects(&lock));
	assert(length);
	assert(!(length % kPageSize));

	if(offset + length > view->length())
		return kErrBufferTooSmall;

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

	// TODO: This is a hack to "upgrade" CoW@fork to CoW.
	// TODO: Remove this once we remove CopyOnWriteAtFork.
	if(flags & kMapCopyOnWriteAtFork)
		flags |= kMapCopyOnWrite;
	if((flags & kMapCopyOnWrite) || (flags & kMapCopyOnWriteAtFork))
		mapping_flags |= MappingFlags::copyOnWriteAtFork;

	if(flags & kMapDropAtFork) {
		mapping_flags |= MappingFlags::dropAtFork;
	}else if(flags & kMapShareAtFork) {
		mapping_flags |= MappingFlags::shareAtFork;
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

	Mapping *mapping;
	if(flags & kMapCopyOnWrite) {
		auto chain = frigg::makeShared<CowChain>(*kernelAlloc, view.toShared(), offset, length);
		auto ptr = smarter::allocate_shared<CowMapping>(Allocator{}, selfPtr.lock(),
				target, length, static_cast<MappingFlags>(mapping_flags), frigg::move(chain));
		ptr->selfPtr = ptr;
		mapping = ptr.get();
		ptr.release(); // AddressSpace owns one reference.
	}else{
		auto ptr = smarter::allocate_shared<NormalMapping>(Allocator{}, selfPtr.lock(),
				target, length, static_cast<MappingFlags>(mapping_flags), view.toShared(), offset);
		ptr->selfPtr = ptr;
		mapping = ptr.get();
		ptr.release(); // AddressSpace owns one reference.
	}

	// Install the new mapping object.
	_mappings.insert(mapping);
	assert(!(flags & kMapPopulate));
	mapping->install(false);

	*actual_address = target;
	return kErrSuccess;
}

bool AddressSpace::unmap(VirtualAddr address, size_t length, AddressUnmapNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	AddressSpace::Guard space_guard(&lock);

	Mapping *mapping = _getMapping(address);
	assert(mapping);

	// TODO: Allow shrinking of the mapping.
	assert(mapping->address() == address);
	assert(mapping->length() == length);
	mapping->uninstall(true);

	static constexpr auto deleteMapping = [] (AddressSpace *space, Mapping *mapping) {
		space->_mappings.remove(mapping);
		mapping->retire();
		mapping->selfPtr.ctr()->decrement();
	};

	static constexpr auto closeHole = [] (AddressSpace *space, VirtualAddr address, size_t length) {
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
		AddressSpace::Guard space_guard(&node->_space->lock);

		deleteMapping(node->_space, node->_mapping);
		closeHole(node->_space, node->_shootNode.address, node->_shootNode.size);
		node->complete();
	});

	node->_space = this;
	node->_mapping = mapping;
	node->_shootNode.address = address;
	node->_shootNode.size = length;
	node->_shootNode.setup(&node->_worklet);
	if(!_pageSpace.submitShootdown(&node->_shootNode))
		return false;

	deleteMapping(this, mapping);
	closeHole(this, address, length);
	return true;
}

bool AddressSpace::handleFault(VirtualAddr address, uint32_t fault_flags, FaultNode *node) {
	node->_address = address;
	node->_flags = fault_flags;

	Mapping *mapping;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&lock);

		mapping = _getMapping(address);
		if(!mapping) {
			node->_resolved = false;
			return true;
		}
	}

	// FIXME: mapping might be deleted here!
	// We need to use either refcounting or QS garbage collection here!

	node->_mapping = mapping;

	// Here we do the mapping-based fault handling.
	if(node->_flags & AddressSpace::kFaultWrite)
		if(!((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)) {
			node->_resolved = false;
			return true;
		}
	if(node->_flags & AddressSpace::kFaultExecute)
		if(!((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)) {
			node->_resolved = false;
			return true;
		}

	struct Ops {
		static void prepared(Worklet *base) {
			auto node = frg::container_of(base, &FaultNode::_worklet);
			update(node);
			WorkQueue::post(node->_handled);
		}

		static void update(FaultNode *node) {
			auto mapping = node->_mapping;

			auto fault_page = (node->_address - mapping->address()) & ~(kPageSize - 1);
			auto vaddr = mapping->address() + fault_page;
			// TODO: This can actually happen!
			assert(!mapping->owner()->_pageSpace.isMapped(vaddr));

			uint32_t page_flags = 0;
			if((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)
				page_flags |= page_access::write;
			if((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)
				page_flags |= page_access::execute;
			// TODO: Allow inaccessible mappings.
			assert((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protRead);

			auto range = mapping->resolveRange(fault_page);
			assert(range.get<0>() != PhysicalAddr(-1));

			mapping->owner()->_pageSpace.mapSingle4k(vaddr, range.get<0>(),
					true, page_flags, range.get<1>());
			mapping->owner()->_residuentSize += kPageSize;
			logRss(mapping->owner());
			node->_resolved = true;
		}
	};

	auto fault_page = (node->_address - mapping->address()) & ~(kPageSize - 1);
	node->_worklet.setup(Ops::prepared);
	node->_touchVirtual.setup(fault_page, &node->_worklet);
	if(mapping->touchVirtualPage(&node->_touchVirtual)) {
		Ops::update(node);
		return true;
	}else{
		return false;
	}
}

bool AddressSpace::fork(ForkNode *node) {
	node->_fork = AddressSpace::create();
	node->_original = this;

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&this->lock);

	// Copy holes to the child space.
	auto cur_hole = _holes.first();
	while(cur_hole) {
		auto fork_hole = frigg::construct<Hole>(*kernelAlloc,
				cur_hole->address(), cur_hole->length());
		node->_fork->_holes.insert(fork_hole);

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
			node->_fork->_holes.insert(fork_hole);
		}else if(cur_mapping->flags() & MappingFlags::shareAtFork) {
			auto fork_mapping = cur_mapping->shareMapping(node->_fork->selfPtr.lock());

			node->_fork->_mappings.insert(fork_mapping);
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
				auto origin_mapping = cur_mapping->copyOnWrite(selfPtr.lock());
				auto fork_mapping = cur_mapping->copyOnWrite(node->_fork->selfPtr.lock());

				// TODO: We have to perform shootdown if we remap the mapping.
				_mappings.remove(cur_mapping);
				_mappings.insert(origin_mapping);
				node->_fork->_mappings.insert(fork_mapping);
				cur_mapping->uninstall(false);
				origin_mapping->install(true);
				fork_mapping->install(false);
				cur_mapping->retire(); // TODO: Shootdown before doing this.
				cur_mapping->selfPtr.ctr()->decrement();
			}else{
				auto bundle = frigg::makeShared<AllocatedMemory>(*kernelAlloc,
						cur_mapping->length(), kPageSize, kPageSize);

				node->_items.addBack(ForkItem{cur_mapping, bundle.get()});

				auto view = frigg::makeShared<MemorySlice>(*kernelAlloc,
						frigg::move(bundle), 0, cur_mapping->length());

				auto ptr = smarter::allocate_shared<NormalMapping>(Allocator{},
						node->_fork->selfPtr.lock(), cur_mapping->address(), cur_mapping->length(),
						cur_mapping->flags(), frigg::move(view), 0);
				ptr->selfPtr = ptr;
				auto fork_mapping = ptr.get();
				ptr.release(); // AddressSpace owns one reference.
				node->_fork->_mappings.insert(fork_mapping);
				fork_mapping->install(false);
			}
		}else{
			assert(!"Illegal mapping type");
		}

		cur_mapping = successor;
	}

	// TODO: Unlocking here should not be necessary.
	lock.unlock();
	irq_lock.unlock();

	node->_progress = 0;

	struct Ops {
		static bool process(ForkNode *node) {
			while(!node->_items.empty()) {
				auto item = &node->_items.front();
				if(node->_progress == item->mapping->length()) {
					node->_items.removeFront();
					node->_progress = 0;
					continue;
				}
				assert(node->_progress < item->mapping->length());
				assert(node->_progress + kPageSize <= item->mapping->length());

				node->_worklet.setup(&Ops::prepared);
				node->_prepare.setup(node->_progress, kPageSize, &node->_worklet);
				if(!item->mapping->populateVirtualRange(&node->_prepare))
					return false;
				doCopy(node);
			}

			return true;
		}

		static void prepared(Worklet *base) {
			auto node = frg::container_of(base, &ForkNode::_worklet);
			doCopy(node);
			if(!process(node))
				return;

			WorkQueue::post(node->_forked);
		}

		static void doCopy(ForkNode *node) {
			auto item = &node->_items.front();
			auto range = item->mapping->resolveRange(node->_progress);
			assert(range.get<0>() != PhysicalAddr(-1));

			PageAccessor accessor{range.get<0>()};
			item->destBundle->copyKernelToThisSync(node->_progress, accessor.get(), kPageSize);
			node->_progress += kPageSize;
		}
	};

	return Ops::process(node);
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

ForeignSpaceAccessor::ForeignSpaceAccessor(smarter::shared_ptr<AddressSpace, BindableHandle> space,
		void *pointer, size_t length)
: _space{std::move(space)}, _address{reinterpret_cast<uintptr_t>(pointer)}, _length{length} {
	if(!_length)
		return;
	assert(_address);

	auto irq_lock = frigg::guard(&irqMutex());
	AddressSpace::Guard guard(&_space->lock);

	// TODO: Verify the mapping's size.
	auto mapping = _space->_getMapping(_address);
	assert(mapping);
	_mapping = mapping->selfPtr.lock();
}

ForeignSpaceAccessor::~ForeignSpaceAccessor() {
	if(!_length)
		return;

	if(_active)
		_mapping->unlockVirtualRange(_address - _mapping->address(), _length);
}

bool ForeignSpaceAccessor::acquire(AcquireNode *node) {
	if(!_length) {
		_active = true;
		return true;
	}

	node->_accessor = this;

	struct Ops {
		static bool doLock(AcquireNode *node) {
			auto self = node->_accessor;
			node->_lockNode.setup(self->_address - self->_mapping->address(), self->_length,
					&node->_worklet);
			node->_worklet.setup([] (Worklet *base) {
				auto node = frg::container_of(base, &AcquireNode::_worklet);
				auto self = node->_accessor;
				if(doPopulate(node)) {
					self->_active = true;
					WorkQueue::post(node->_acquired);
				}
			});
			if(!self->_mapping->lockVirtualRange(&node->_lockNode))
				return false;
			return doPopulate(node);
		}

		static bool doPopulate(AcquireNode *node) {
			auto self = node->_accessor;
			node->_populateNode.setup(self->_address - self->_mapping->address(), self->_length,
					&node->_worklet);
			node->_worklet.setup([] (Worklet *base) {
				auto node = frg::container_of(base, &AcquireNode::_worklet);
				auto self = node->_accessor;
				self->_active = true;
				WorkQueue::post(node->_acquired);
			});
			if(!self->_mapping->populateVirtualRange(&node->_populateNode))
				return false;
			return true;
		}
	};

	if(!Ops::doLock(node))
		return false;

	_active = true;
	return true;
}

PhysicalAddr ForeignSpaceAccessor::getPhysical(size_t offset) {
	return _resolvePhysical(_address + offset);
}

void ForeignSpaceAccessor::load(size_t offset, void *pointer, size_t size) {
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

Error ForeignSpaceAccessor::write(size_t offset, const void *pointer, size_t size) {
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

PhysicalAddr ForeignSpaceAccessor::_resolvePhysical(VirtualAddr vaddr) {
	auto range = _mapping->resolveRange(vaddr - _mapping->address());
	return range.get<0>();
}

} // namespace thor

