#include "execution/coroutine.hpp"
#include "fiber.hpp"
#include "memory-view.hpp"
#include "physical.hpp"
#include "service_helpers.hpp"

namespace thor {

namespace {
	constexpr bool logUsage = false;
	constexpr bool logUncaching = false;

	// The following flags are debugging options to debug the correctness of various components.
	constexpr bool disableUncaching = false;
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

		// The reclaimer owns one reference to the page.
		// This ensures that it can safely initiate uncaching operations.
		page->refcount.fetch_add(1, std::memory_order_acq_rel);

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

		if(page->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
			page->bundle->retirePage(page);
	}

	KernelFiber *createReclaimFiber() {
		auto checkReclaim = [this] () -> bool {
			if(disableUncaching)
				return false;

			// Take a single page out of the LRU list.
			// TODO: We have to acquire a refcount here.
			CachePage *page;
			{
				auto irq_lock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&_mutex);

				if(_cachedSize <= (1 << 20))
					return false;
				if(_lruList.empty())
					return false;

				page = _lruList.pop_front();

				// Take another reference while we do the uncaching. (removePage() could be
				// called concurrently and release the reclaimer's reference).
				page->refcount.fetch_add(1, std::memory_order_acq_rel);

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

			if(page->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
				page->bundle->retirePage(page);

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

void MemoryView::copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t size) {
	(void)offset;
	(void)pointer;
	(void)size;
	frigg::panicLogger() << "MemoryView does not support synchronous operations!" << frigg::endLog;
}

void MemoryView::resize(size_t newSize, execution::any_receiver<void> receiver) {
	(void)newSize;
	(void)receiver;
	frigg::panicLogger() << "MemoryView does not support resize!" << frigg::endLog;
}

void MemoryView::fork(execution::any_receiver<frg::tuple<Error, frigg::SharedPtr<MemoryView>>> receiver) {
	receiver.set_value({kErrIllegalObject, nullptr});
}

void MemoryView::asyncLockRange(uintptr_t offset, size_t size,
		execution::any_receiver<Error> receiver) {
	receiver.set_value(lockRange(offset, size));
}

Error MemoryView::updateRange(ManageRequest type, size_t offset, size_t length) {
	return kErrIllegalObject;
}

void MemoryView::submitManage(ManageNode *handle) {
	frigg::panicLogger() << "MemoryView does not support management!" << frigg::endLog;
}

void MemoryView::submitInitiateLoad(MonitorNode *initiate) {
	initiate->setup(kErrSuccess);
	initiate->complete();
}

Error MemoryView::setIndirection(size_t slot, frigg::SharedPtr<MemoryView> view,
		uintptr_t offset, size_t size) {
	return kErrIllegalObject;
}

// --------------------------------------------------------
// Copy operations.
// --------------------------------------------------------

bool transferBetweenViews(TransferNode *node) {
	node->_progress = 0;

	if(auto e = node->_srcBundle->lockRange(node->_srcOffset, node->_size); e)
		assert(!"lockRange() failed");
	if(auto e = node->_destBundle->lockRange(node->_destOffset, node->_size); e)
		assert(!"lockRange() failed");

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
			assert(!node->_destFetch.error());
			assert(!node->_srcFetch.error());

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

bool copyToBundle(MemoryView *view, ptrdiff_t offset, const void *pointer, size_t size,
		CopyToBundleNode *node, void (*complete)(CopyToBundleNode *)) {
	size_t progress = 0;
	size_t misalign = offset % kPageSize;

	if(auto e = view->lockRange(offset, size); e)
		assert(!"lockRange() failed");

	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, size);

		node->_worklet.setup(nullptr);
		node->_fetch.setup(&node->_worklet);
		if(!view->fetchRange(offset - misalign, &node->_fetch))
			assert(!"Handle the asynchronous case");
		assert(!node->_fetch.error());

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
		assert(!node->_fetch.error());

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
		assert(!node->_fetch.error());

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

				// Tail of asynchronous path.
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
			assert(!node->_fetch.error());
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
	if(auto e = view->lockRange(offset, size); e)
		assert(!"lockRange() failed");

	return Ops::process(node);
}

// --------------------------------------------------------
// HardwareMemory
// --------------------------------------------------------

HardwareMemory::HardwareMemory(PhysicalAddr base, size_t length, CachingMode cache_mode)
: _base{base}, _length{length}, _cacheMode{cache_mode} {
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

Error HardwareMemory::lockRange(uintptr_t offset, size_t size) {
	// Hardware memory is "always locked".
	return kErrSuccess;
}

void HardwareMemory::unlockRange(uintptr_t offset, size_t size) {
	// Hardware memory is "always locked".
}

frg::tuple<PhysicalAddr, CachingMode> HardwareMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	return frg::tuple<PhysicalAddr, CachingMode>{_base + offset, _cacheMode};
}

bool HardwareMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	assert(offset % kPageSize == 0);

	completeFetch(node, kErrSuccess, _base + offset, _length - offset, _cacheMode);
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

AllocatedMemory::AllocatedMemory(size_t desiredLngth,
		int addressBits, size_t desiredChunkSize, size_t chunkAlign)
: _physicalChunks{*kernelAlloc},
		_addressBits{addressBits}, _chunkAlign{chunkAlign} {
	static_assert(sizeof(unsigned long) == sizeof(uint64_t), "Fix use of __builtin_clzl");
	_chunkSize = size_t(1) << (64 - __builtin_clzl(desiredChunkSize - 1));
	if(_chunkSize != desiredChunkSize)
		frigg::infoLogger() << "\e[31mPhysical allocation of size " << (void *)desiredChunkSize
				<< " rounded up to power of 2\e[39m" << frigg::endLog;

	size_t length = (desiredLngth + (_chunkSize - 1)) & ~(_chunkSize - 1);
	if(length != desiredLngth)
		frigg::infoLogger() << "\e[31mMemory length " << (void *)desiredLngth
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

void AllocatedMemory::resize(size_t newSize, execution::any_receiver<void> receiver) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(!(newSize % _chunkSize));
	size_t num_chunks = newSize / _chunkSize;
	assert(num_chunks >= _physicalChunks.size());
	_physicalChunks.resize(num_chunks, PhysicalAddr(-1));
	receiver.set_value();
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
		auto physical = physicalAllocator->allocate(_chunkSize, _addressBits);
		assert(physical != PhysicalAddr(-1) && "OOM");
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

Error AllocatedMemory::lockRange(uintptr_t offset, size_t size) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
	return kErrSuccess;
}

void AllocatedMemory::unlockRange(uintptr_t offset, size_t size) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
}

frg::tuple<PhysicalAddr, CachingMode> AllocatedMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	auto index = offset / _chunkSize;
	auto disp = offset & (_chunkSize - 1);
	assert(index < _physicalChunks.size());

	if(_physicalChunks[index] == PhysicalAddr(-1))
		return frg::tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
	return frg::tuple<PhysicalAddr, CachingMode>{_physicalChunks[index] + disp,
			CachingMode::null};
}

bool AllocatedMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	auto index = offset / _chunkSize;
	auto disp = offset & (_chunkSize - 1);
	assert(index < _physicalChunks.size());

	if(_physicalChunks[index] == PhysicalAddr(-1)) {
		auto physical = physicalAllocator->allocate(_chunkSize, _addressBits);
		assert(physical != PhysicalAddr(-1) && "OOM");
		assert(!(physical & (_chunkAlign - 1)));

		for(size_t pg_progress = 0; pg_progress < _chunkSize; pg_progress += kPageSize) {
			PageAccessor accessor{physical + pg_progress};
			memset(accessor.get(), 0, kPageSize);
		}
		_physicalChunks[index] = physical;
	}

	assert(_physicalChunks[index] != PhysicalAddr(-1));
	completeFetch(node, kErrSuccess,
			_physicalChunks[index] + disp, _chunkSize - disp, CachingMode::null);
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
: pages{*kernelAlloc}, numPages{length >> kPageShift} {
	assert(!(length & (kPageSize - 1)));
}

ManagedSpace::~ManagedSpace() {
	// TODO: Free all physical memory.
	// TODO: We also have to remove all Loaded/Evicting pages from the reclaimer.
	assert(!"Implement this");
}

bool ManagedSpace::uncachePage(CachePage *page, ReclaimNode *continuation) {
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex);

	size_t index = page->identity;
	auto pit = pages.find(index);
	assert(pit);
	assert(pit->loadState == kStatePresent);
	pit->loadState = kStateEvicting;
	globalReclaimer->removePage(&pit->cachePage);

	execution::detach([] (ManagedSpace *self, CachePage *page, ManagedPage *pit,
			ReclaimNode *continuation) -> coroutine<void> {
		co_await self->_evictQueue.evictRange(page->identity << kPageShift, kPageSize);

		auto irqLock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&self->mutex);

		if(pit->loadState != kStateEvicting)
			co_return;
		assert(!pit->lockCount);

		if(logUncaching)
			frigg::infoLogger() << "\e[33mEvicting physical page\e[39m" << frigg::endLog;
		assert(pit->physical != PhysicalAddr(-1));
		physicalAllocator->free(pit->physical, kPageSize);
		pit->loadState = kStateMissing;
		pit->physical = PhysicalAddr(-1);
		continuation->complete();
	}(this, page, pit, continuation));
	return false;
}

void ManagedSpace::retirePage(CachePage *page) {
	// TODO: Take a reference to the CachePage when it is first used.
	//       Take a reference to the ManagedSpace for each CachePage in use (so that it is not
	//       destructed until all CachePages are retired).
}

// Note: Neither offset nor size are necessarily multiples of the page size.
Error ManagedSpace::lockPages(uintptr_t offset, size_t size) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex);
	if((offset + size) / kPageSize > numPages)
		return kErrBufferTooSmall;

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		size_t index = (offset + pg) / kPageSize;
		auto [pit, wasInserted] = pages.find_or_insert(index, this, index);
		assert(pit);
		pit->lockCount++;
		if(pit->lockCount == 1) {
			if(pit->loadState == kStatePresent) {
				globalReclaimer->removePage(&pit->cachePage);
			}else if(pit->loadState == kStateEvicting) {
				// Stop the eviction to keep the page present.
				pit->loadState = kStatePresent;
			}
		}
		assert(pit->loadState != kStateEvicting);
	}
	return kErrSuccess;
}

// Note: Neither offset nor size are necessarily multiples of the page size.
void ManagedSpace::unlockPages(uintptr_t offset, size_t size) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex);
	assert((offset + size) / kPageSize <= numPages);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		size_t index = (offset + pg) / kPageSize;
		auto pit = pages.find(index);
		assert(pit);
		assert(pit->lockCount > 0);
		pit->lockCount--;
		if(!pit->lockCount) {
			if(pit->loadState == kStatePresent) {
				globalReclaimer->addPage(&pit->cachePage);
			}
		}
		assert(pit->loadState != kStateEvicting);
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
	assert((node->offset + node->length) / kPageSize <= numPages);

	_monitorQueue.push_back(node);
	_progressMonitors();
}

void ManagedSpace::_progressManagement() {
	// For now, we prefer writeback to initialization.
	// "Proper" priorization should probably be done in the userspace driver
	// (we do not want to store per-page priorities here).

	while(!_writebackList.empty() && !_managementQueue.empty()) {
		auto page = _writebackList.front();
		auto index = page->identity;

		// Fuse the request with adjacent pages in the list.
		ptrdiff_t count = 0;
		while(!_writebackList.empty()) {
			auto fuse_cache_page = _writebackList.front();
			auto fuse_index = fuse_cache_page->identity;
			auto fuse_managed_page = frg::container_of(fuse_cache_page, &ManagedPage::cachePage);
			if(fuse_index != index + count)
				break;
			assert(fuse_managed_page->loadState == kStateWantWriteback);
			fuse_managed_page->loadState = kStateWriteback;
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
		auto index = page->identity;

		// Fuse the request with adjacent pages in the list.
		ptrdiff_t count = 0;
		while(!_initializationList.empty()) {
			auto fuse_cache_page = _initializationList.front();
			auto fuse_index = fuse_cache_page->identity;
			auto fuse_managed_page = frg::container_of(fuse_cache_page, &ManagedPage::cachePage);
			if(fuse_index != index + count)
				break;
			assert(fuse_managed_page->loadState == kStateWantInitialization);
			fuse_managed_page->loadState = kStateInitialization;
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
			auto pit = pages.find(index);
			assert(pit);
			if(pit->loadState == kStateWantInitialization
					|| pit->loadState == kStateInitialization)
				return false;

			assert(pit->loadState == kStatePresent
					|| pit->loadState == kStateWantWriteback
					|| pit->loadState == kStateWriteback
					|| pit->loadState == kStateAnotherWriteback
					|| pit->loadState == kStateEvicting);
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

void BackingMemory::resize(size_t newSize, execution::any_receiver<void> receiver) {
	assert(!(newSize & (kPageSize - 1)));
	auto newPages = newSize >> kPageShift;

	execution::detach([] (BackingMemory *self, size_t newPages,
			execution::any_receiver<void> receiver) -> coroutine<void> {
		auto irqLock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&self->_managed->mutex);

		if(newPages > self->_managed->numPages) {
			// Do nothing for now.
		}else if(newPages < self->_managed->numPages) {
			co_await self->_managed->_evictQueue.evictRange(newPages << kPageShift,
					self->_managed->numPages << kPageShift);
			// TODO: also free the affected pages!
		}
		self->_managed->numPages = newPages;

		receiver.set_value();
	}(this, newPages, std::move(receiver)));
}

void BackingMemory::addObserver(smarter::shared_ptr<MemoryObserver> observer) {
	_managed->_evictQueue.addObserver(std::move(observer));
}

void BackingMemory::removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) {
	_managed->_evictQueue.removeObserver(observer);
}

Error BackingMemory::lockRange(uintptr_t offset, size_t size) {
	return _managed->lockPages(offset, size);
}

void BackingMemory::unlockRange(uintptr_t offset, size_t size) {
	_managed->unlockPages(offset, size);
}

frg::tuple<PhysicalAddr, CachingMode> BackingMemory::peekRange(uintptr_t offset) {
	assert(!(offset % kPageSize));

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->numPages);
	auto pit = _managed->pages.find(index);

	if(!pit)
		return frg::tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
	return frg::tuple<PhysicalAddr, CachingMode>{pit->physical, CachingMode::null};
}

bool BackingMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);
	assert(index < _managed->numPages);
	auto [pit, wasInserted] = _managed->pages.find_or_insert(index, _managed.get(), index);
	assert(pit);

	if(pit->physical == PhysicalAddr(-1)) {
		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		assert(physical != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor{physical};
		memset(accessor.get(), 0, kPageSize);
		pit->physical = physical;
	}

	completeFetch(node, kErrSuccess, pit->physical + misalign, kPageSize - misalign,
			CachingMode::null);
	return true;
}

void BackingMemory::markDirty(uintptr_t offset, size_t size) {
	// Writes through the BackingMemory do not affect the dirty state!
}

size_t BackingMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->numPages << kPageShift;
}

void BackingMemory::submitManage(ManageNode *node) {
	_managed->submitManagement(node);
}

Error BackingMemory::updateRange(ManageRequest type, size_t offset, size_t length) {
	assert((offset % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);
	assert((offset + length) / kPageSize <= _managed->numPages);

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
			auto pit = _managed->pages.find(index);
			assert(pit);
			assert(pit->loadState == ManagedSpace::kStateInitialization);
			pit->loadState = ManagedSpace::kStatePresent;
			if(!pit->lockCount)
				globalReclaimer->addPage(&pit->cachePage);
		}
	}else{
		for(size_t pg = 0; pg < length; pg += kPageSize) {
			size_t index = (offset + pg) / kPageSize;
			auto pit = _managed->pages.find(index);
			assert(pit);

			if(pit->loadState == ManagedSpace::kStateWriteback) {
				pit->loadState = ManagedSpace::kStatePresent;
				if(!pit->lockCount)
					globalReclaimer->addPage(&pit->cachePage);
			}else{
				assert(pit->loadState == ManagedSpace::kStateAnotherWriteback);
				pit->loadState = ManagedSpace::kStateWantWriteback;
				_managed->_writebackList.push_back(&pit->cachePage);
			}
		}
	}

	_managed->_progressMonitors();

	return kErrSuccess;
}

// --------------------------------------------------------
// FrontalMemory
// --------------------------------------------------------

void FrontalMemory::addObserver(smarter::shared_ptr<MemoryObserver> observer) {
	_managed->_evictQueue.addObserver(std::move(observer));
}

void FrontalMemory::removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) {
	_managed->_evictQueue.removeObserver(observer);
}

Error FrontalMemory::lockRange(uintptr_t offset, size_t size) {
	return _managed->lockPages(offset, size);
}

void FrontalMemory::unlockRange(uintptr_t offset, size_t size) {
	_managed->unlockPages(offset, size);
}

frg::tuple<PhysicalAddr, CachingMode> FrontalMemory::peekRange(uintptr_t offset) {
	assert(!(offset % kPageSize));

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->numPages);
	auto pit = _managed->pages.find(index);

	if(!pit || pit->loadState != ManagedSpace::kStatePresent)
		return frg::tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
	return frg::tuple<PhysicalAddr, CachingMode>{pit->physical, CachingMode::null};
}

bool FrontalMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_managed->mutex);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);
	assert(index < _managed->numPages);

	// Try the fast-paths first.
	auto [pit, wasInserted] = _managed->pages.find_or_insert(index, _managed.get(), index);
	assert(pit);
	if(pit->loadState == ManagedSpace::kStatePresent
			|| pit->loadState == ManagedSpace::kStateWantWriteback
			|| pit->loadState == ManagedSpace::kStateWriteback
			|| pit->loadState == ManagedSpace::kStateAnotherWriteback
			|| pit->loadState == ManagedSpace::kStateEvicting) {
		auto physical = pit->physical;
		assert(physical != PhysicalAddr(-1));

		if(pit->loadState == ManagedSpace::kStatePresent) {
			if(!pit->lockCount)
				globalReclaimer->bumpPage(&pit->cachePage);
		}else if(pit->loadState == ManagedSpace::kStateEvicting) {
			// Cancel evication -- the page is still needed.
			pit->loadState = ManagedSpace::kStatePresent;
			globalReclaimer->addPage(&pit->cachePage);
		}

		completeFetch(node, kErrSuccess,
				physical + misalign, kPageSize - misalign, CachingMode::null);
		return true;
	}else{
		assert(pit->loadState == ManagedSpace::kStateMissing
				|| pit->loadState == ManagedSpace::kStateWantInitialization
				|| pit->loadState == ManagedSpace::kStateInitialization);
	}

	if(node->flags() & FetchNode::disallowBacking) {
		frigg::infoLogger() << "\e[31m" "thor: Backing of page is disallowed" "\e[39m"
				<< frigg::endLog;
		completeFetch(node, kErrFault);
		return true;
	}

	// We have to take the slow-path, i.e., perform the fetch asynchronously.
	if(pit->loadState == ManagedSpace::kStateMissing) {
		pit->loadState = ManagedSpace::kStateWantInitialization;
		_managed->_initializationList.push_back(&pit->cachePage);
	}
	_managed->_progressManagement();

	// TODO: Do not allocate memory here; use pre-allocated nodes instead.
	struct Closure {
		uintptr_t offset;
		ManagedSpace::ManagedPage *page;
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

			auto misalign = closure->offset & (kPageSize - 1);
			assert(closure->page->loadState == ManagedSpace::kStatePresent);
			auto physical = closure->page->physical;
			assert(physical != PhysicalAddr(-1));

			lock.unlock();
			irq_lock.unlock();

			completeFetch(closure->fetch, kErrSuccess, physical + misalign, kPageSize - misalign,
					CachingMode::null);
			callbackFetch(closure->fetch);
			frigg::destruct(*kernelAlloc, closure);
		}
	};

	closure->offset = offset;
	closure->page = pit;
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
		auto pit = _managed->pages.find(index);
		assert(pit);
		if(pit->loadState == ManagedSpace::kStatePresent) {
			pit->loadState = ManagedSpace::kStateWantWriteback;
			if(!pit->lockCount)
				globalReclaimer->removePage(&pit->cachePage);
			_managed->_writebackList.push_back(&pit->cachePage);
		}else if(pit->loadState == ManagedSpace::kStateWriteback) {
			pit->loadState = ManagedSpace::kStateAnotherWriteback;
		}else{
			assert(pit->loadState == ManagedSpace::kStateWantWriteback
					|| pit->loadState == ManagedSpace::kStateAnotherWriteback);
			return;
		}
	}

	_managed->_progressManagement();
}

size_t FrontalMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->numPages << kPageShift;
}

void FrontalMemory::submitInitiateLoad(MonitorNode *node) {
	// TODO: This assumes that we want to load the range (which might not be true).
	assert(node->offset % kPageSize == 0);
	assert(node->length % kPageSize == 0);
	for(size_t pg = 0; pg < node->length; pg += kPageSize) {
		auto index = (node->offset + pg) >> kPageShift;
		auto [pit, wasInserted] = _managed->pages.find_or_insert(index, _managed.get(), index);
		assert(pit);
		if(pit->loadState == ManagedSpace::kStateMissing) {
			pit->loadState = ManagedSpace::kStateWantInitialization;
			_managed->_initializationList.push_back(&pit->cachePage);
		}
	}
	_managed->_progressManagement();

	_managed->submitMonitor(node);
}

// --------------------------------------------------------
// IndirectMemory
// --------------------------------------------------------

IndirectMemory::IndirectMemory(size_t numSlots)
: indirections_{*kernelAlloc} {
	indirections_.resize(numSlots);
}

IndirectMemory::~IndirectMemory() {
	// For now we do nothing when deallocating hardware memory.
}

void IndirectMemory::addObserver(smarter::shared_ptr<MemoryObserver>) {
	// As we never evict memory, there is no need to handle observers.
}

void IndirectMemory::removeObserver(smarter::borrowed_ptr<MemoryObserver>) {
	// As we never evict memory, there is no need to handle observers.
}

Error IndirectMemory::lockRange(uintptr_t offset, size_t size) {
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex_);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);
	if(slot >= indirections_.size())
		return kErrFault;
	if(!indirections_[slot])
		return kErrFault;
	if(inSlotOffset + size > indirections_[slot]->size)
		return kErrFault;
	return indirections_[slot]->memory->lockRange(indirections_[slot]->offset
			+ inSlotOffset, size);
}

void IndirectMemory::unlockRange(uintptr_t offset, size_t size) {
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex_);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);
	assert(slot < indirections_.size()); // TODO: Return kErrFault.
	assert(indirections_[slot]); // TODO: Return kErrFault.
	assert(inSlotOffset + size <= indirections_[slot]->size); // TODO: Return kErrFault.
	return indirections_[slot]->memory->unlockRange(indirections_[slot]->offset
			+ inSlotOffset, size);
}

frg::tuple<PhysicalAddr, CachingMode> IndirectMemory::peekRange(uintptr_t offset) {
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex_);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);
	assert(slot < indirections_.size()); // TODO: Return kErrFault.
	assert(indirections_[slot]); // TODO: Return kErrFault.
	return indirections_[slot]->memory->peekRange(indirections_[slot]->offset
			+ inSlotOffset);
}

bool IndirectMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex_);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);
	assert(slot < indirections_.size()); // TODO: Return kErrFault.
	assert(indirections_[slot]); // TODO: Return kErrFault.
	return indirections_[slot]->memory->fetchRange(indirections_[slot]->offset
			+ inSlotOffset, node);
}

void IndirectMemory::markDirty(uintptr_t offset, size_t size) {
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex_);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);
	assert(slot < indirections_.size()); // TODO: Return kErrFault.
	assert(indirections_[slot]); // TODO: Return kErrFault.
	assert(inSlotOffset + size <= indirections_[slot]->size); // TODO: Return kErrFault.
	indirections_[slot]->memory->markDirty(indirections_[slot]->offset
			+ inSlotOffset, size);
}

size_t IndirectMemory::getLength() {
	return indirections_.size() << 32;
}

Error IndirectMemory::setIndirection(size_t slot, frigg::SharedPtr<MemoryView> memory,
		uintptr_t offset, size_t size) {
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex_);

	if(slot >= indirections_.size())
		return kErrOutOfBounds;
	auto indirection = smarter::allocate_shared<IndirectionSlot>(*kernelAlloc,
			this, slot, memory, offset, size);
	memory->addObserver(smarter::shared_ptr<MemoryObserver>{indirection, &indirection->observer});
	indirections_[slot] = std::move(indirection);
	return kErrSuccess;
}

bool IndirectMemory::SlotObserver::observeEviction(uintptr_t offset, size_t length,
		EvictNode *node) {
	assert(!"TODO: implement eviction of IndirectMemory");
	__builtin_trap();
}

// --------------------------------------------------------
// CopyOnWriteMemory
// --------------------------------------------------------

CopyOnWriteMemory::CopyOnWriteMemory(frigg::SharedPtr<MemoryView> view,
		uintptr_t offset, size_t length,
		frigg::SharedPtr<CowChain> chain)
: _view{std::move(view)}, _viewOffset{offset}, _length{length}, _copyChain{std::move(chain)},
		_ownedPages{*kernelAlloc} {
	assert(length);
	assert(!(offset & (kPageSize - 1)));
	assert(!(length & (kPageSize - 1)));
}

CopyOnWriteMemory::~CopyOnWriteMemory() {
	for(auto it = _ownedPages.begin(); it != _ownedPages.end(); ++it) {
		assert(it->state == CowState::hasCopy);
		assert(it->physical != PhysicalAddr(-1));
		physicalAllocator->free(it->physical, kPageSize);
	}
}

size_t CopyOnWriteMemory::getLength() {
	return _length;
}

void CopyOnWriteMemory::fork(execution::any_receiver<frg::tuple<Error, frigg::SharedPtr<MemoryView>>> receiver) {
	// Note that locked pages require special attention during CoW: as we cannot
	// replace them by copies, we have to copy them eagerly.
	// Therefore, they are special-cased below.

	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	// Create a new CowChain for both the original and the forked mapping.
	// To correct handle locks pages, we move only non-locked pages from
	// the original mapping to the new chain.
	auto newChain = frigg::makeShared<CowChain>(*kernelAlloc, _copyChain);

	// Update the original mapping
	_copyChain = newChain;

	// Create a new mapping in the forked space.
	auto forked = frigg::makeShared<CopyOnWriteMemory>(*kernelAlloc,
			_view, _viewOffset, _length, newChain);

	// Finally, inspect all copied pages owned by the original mapping.
	for(size_t pg = 0; pg < _length; pg += kPageSize) {
		auto osIt = _ownedPages.find(pg >> kPageShift);

		if(!osIt)
			continue;
		assert(osIt->state == CowState::hasCopy);

		// The page is locked. We *need* to keep it in the old address space.
		if(osIt->lockCount /*|| disableCow */) {
			// Allocate a new physical page for a copy.
			auto copyPhysical = physicalAllocator->allocate(kPageSize);
			assert(copyPhysical != PhysicalAddr(-1) && "OOM");

			// As the page is locked anyway, we can just copy it synchronously.
			PageAccessor lockedAccessor{osIt->physical};
			PageAccessor copyAccessor{copyPhysical};
			memcpy(copyAccessor.get(), lockedAccessor.get(), kPageSize);

			// Update the chains.
			auto fsIt = forked->_ownedPages.insert(pg >> kPageShift);
			fsIt->state = CowState::hasCopy;
			fsIt->physical = copyPhysical;
		}else{
			auto physical = osIt->physical;
			assert(physical != PhysicalAddr(-1));

			// Update the chains.
			auto pageOffset = _viewOffset + pg;
			auto newIt = newChain->_pages.insert(pageOffset >> kPageShift,
					PhysicalAddr(-1));
			_ownedPages.erase(pg >> kPageShift);
			newIt->store(physical, std::memory_order_relaxed);
		}
	}

	execution::detach([] (CopyOnWriteMemory *self, frigg::SharedPtr<CopyOnWriteMemory> forked,
			execution::any_receiver<frg::tuple<Error, frigg::SharedPtr<MemoryView>>> receiver) -> coroutine<void> {
			co_await self->_evictQueue.evictRange(0, self->_length);
			receiver.set_value({kErrSuccess, std::move(forked)});
	}(this, std::move(forked), receiver));
}

void CopyOnWriteMemory::addObserver(smarter::shared_ptr<MemoryObserver> observer) {
	_evictQueue.addObserver(std::move(observer));
}

void CopyOnWriteMemory::removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) {
	_evictQueue.removeObserver(observer);
}

Error CopyOnWriteMemory::lockRange(uintptr_t, size_t) {
	frigg::panicLogger() << "CopyOnWriteMemory does not support synchronous lockRange()"
			<< frigg::endLog;
	__builtin_unreachable();
}

void CopyOnWriteMemory::asyncLockRange(uintptr_t offset, size_t size,
		execution::any_receiver<Error> receiver) {
	// For now, it is enough to populate the range, as pages can only be evicted from
	// the root of the CoW chain, but copies are never evicted.
	execution::detach([] (CopyOnWriteMemory *self, uintptr_t overallOffset, size_t size,
			execution::any_receiver<Error> receiver) -> coroutine<void> {
		size_t progress = 0;
		while(progress < size) {
			auto offset = overallOffset + progress;

			frigg::SharedPtr<CowChain> chain;
			frigg::SharedPtr<MemoryView> view;
			uintptr_t viewOffset;
			{
				// If the page is present in our private chain, we just return it.
				auto irqLock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&self->_mutex);

				if(auto it = self->_ownedPages.find(offset >> kPageShift); it) {
					assert(it->state == CowState::hasCopy);
					assert(it->physical != PhysicalAddr(-1));

					it->lockCount++;
					progress += kPageSize;
					continue;
				}

				chain = self->_copyChain;
				view = self->_view;
				viewOffset = self->_viewOffset;

				// Otherwise we need to copy from the chain or from the root view.
				auto it = self->_ownedPages.insert(offset >> kPageShift);
				it->state = CowState::inProgress;
			}

			PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
			assert(physical != PhysicalAddr(-1) && "OOM");
			PageAccessor accessor{physical};

			// Try to copy from a descendant CoW chain.
			auto pageOffset = viewOffset + offset;
			while(chain) {
				auto irqLock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&chain->_mutex);

				if(auto it = chain->_pages.find(pageOffset >> kPageShift); it) {
					// We can just copy synchronously here -- the descendant is not evicted.
					auto srcPhysical = it->load(std::memory_order_relaxed);
					assert(srcPhysical != PhysicalAddr(-1));
					auto srcAccessor = PageAccessor{srcPhysical};
					memcpy(accessor.get(), srcAccessor.get(), kPageSize);
					break;
				}

				chain = chain->_superChain;
			}

			// Copy from the root view.
			if(!chain) {
				co_await copyFromView(view.get(), pageOffset & ~(kPageSize - 1),
						accessor.get(), kPageSize);
			}

			// To make CoW unobservable, we first need to evict the page here.
			// TODO: enable read-only eviction.
			co_await self->_evictQueue.evictRange(offset & ~(kPageSize - 1), kPageSize);

			auto irqLock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&self->_mutex);

			auto cowIt = self->_ownedPages.find(offset >> kPageShift);
			assert(cowIt->state == CowState::inProgress);
			cowIt->state = CowState::hasCopy;
			cowIt->physical = physical;
			cowIt->lockCount++;
			progress += kPageSize;
		}

		receiver.set_value(kErrSuccess);
	}(this, offset, size, receiver));
}

void CopyOnWriteMemory::unlockRange(uintptr_t offset, size_t size) {
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto it = _ownedPages.find((offset + pg) >> kPageShift);
		assert(it);
		assert(it->state == CowState::hasCopy);
		assert(it->lockCount > 0);
		it->lockCount--;
	}
}

frg::tuple<PhysicalAddr, CachingMode> CopyOnWriteMemory::peekRange(uintptr_t offset) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	if(auto it = _ownedPages.find(offset >> kPageShift); it) {
		assert(it->state == CowState::hasCopy);
		return frg::tuple<PhysicalAddr, CachingMode>{it->physical, CachingMode::null};
	}

	return frg::tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
}

bool CopyOnWriteMemory::fetchRange(uintptr_t offset, FetchNode *node) {
	execution::detach([] (CopyOnWriteMemory *self, uintptr_t offset,
			FetchNode *node) -> coroutine<void> {
		frigg::SharedPtr<CowChain> chain;
		frigg::SharedPtr<MemoryView> view;
		uintptr_t viewOffset;
		{
			// If the page is present in our private chain, we just return it.
			auto irqLock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&self->_mutex);

			if(auto it = self->_ownedPages.find(offset >> kPageShift); it) {
				assert(it->state == CowState::hasCopy);
				assert(it->physical != PhysicalAddr(-1));

				completeFetch(node, kErrSuccess, it->physical, kPageSize, CachingMode::null);
				callbackFetch(node);
				co_return;
			}

			chain = self->_copyChain;
			view = self->_view;
			viewOffset = self->_viewOffset;

			// Otherwise we need to copy from the chain or from the root view.
			auto it = self->_ownedPages.insert(offset >> kPageShift);
			it->state = CowState::inProgress;
		}

		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		assert(physical != PhysicalAddr(-1) && "OOM");
		PageAccessor accessor{physical};

		// Try to copy from a descendant CoW chain.
		auto pageOffset = viewOffset + offset;
		while(chain) {
			auto irqLock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&chain->_mutex);

			if(auto it = chain->_pages.find(pageOffset >> kPageShift); it) {
				// We can just copy synchronously here -- the descendant is not evicted.
				auto srcPhysical = it->load(std::memory_order_relaxed);
				assert(srcPhysical != PhysicalAddr(-1));
				auto srcAccessor = PageAccessor{srcPhysical};
				memcpy(accessor.get(), srcAccessor.get(), kPageSize);
				break;
			}

			chain = chain->_superChain;
		}

		// Copy from the root view.
		if(!chain) {
			co_await copyFromView(view.get(), pageOffset & ~(kPageSize - 1),
					accessor.get(), kPageSize);
		}

		// To make CoW unobservable, we first need to evict the page here.
		// TODO: enable read-only eviction.
		co_await self->_evictQueue.evictRange(offset, kPageSize);

		auto irqLock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&self->_mutex);

		auto cowIt = self->_ownedPages.find(offset >> kPageShift);
		assert(cowIt->state == CowState::inProgress);
		cowIt->state = CowState::hasCopy;
		cowIt->physical = physical;
		completeFetch(node, kErrSuccess, cowIt->physical, kPageSize, CachingMode::null);
		callbackFetch(node);
	}(this, offset, node));
	return false;
}

void CopyOnWriteMemory::markDirty(uintptr_t, size_t) {
	// We do not need to track dirty pages.
}

} // namespace thor
