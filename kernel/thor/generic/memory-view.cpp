#include <thor-internal/coroutine.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

namespace {
	constexpr bool logUsage = false;
	constexpr bool logUncaching = false;
	constexpr bool tortureUncaching = false;

	// The following flags are debugging options to debug the correctness of various components.
	constexpr bool disableUncaching = false;
}

// --------------------------------------------------------
// Reclaim implementation.
// --------------------------------------------------------

struct MemoryReclaimer {
	void addPage(CachePage *page) {
		// TODO: Do we need the IRQ lock here?
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

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
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

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
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

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

	void runReclaimFiber() {
		auto checkReclaim = [this] () -> bool {
			if(disableUncaching)
				return false;

			// Take a single page out of the LRU list.
			// TODO: We have to acquire a refcount here.
			CachePage *page;
			{
				auto irq_lock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_mutex);

				if(_lruList.empty())
					return false;

				if(!tortureUncaching) {
					auto pagesWatermark = physicalAllocator->numTotalPages() * 3 / 4;
					auto usedPages = physicalAllocator->numUsedPages();
					if(usedPages < pagesWatermark) {
						return false;
					}else{
						if(logUncaching)
							infoLogger() << "thor: Uncaching page. " << usedPages
									<< " pages are in use (watermark: " << pagesWatermark << ")"
									<< frg::endlog;
					}
				}

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
			}, thisFiber()->associatedWorkQueue());

			closure.blocker.setup();
			closure.node.setup(&closure.worklet);
			if(!page->bundle->uncachePage(page, &closure.node))
				KernelFiber::blockCurrent(&closure.blocker);

			if(page->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
				page->bundle->retirePage(page);

			return true;
		};

		KernelFiber::run([=] {
			while(true) {
				if(logUncaching) {
					auto irq_lock = frg::guard(&irqMutex());
					auto lock = frg::guard(&_mutex);
					infoLogger() << "thor: " << (_cachedSize / 1024)
							<< " KiB of cached pages" << frg::endlog;
				}

				while(checkReclaim())
					;
				KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(1'000'000'000));
			}
		});
	}

private:
	frg::ticket_spinlock _mutex;

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

static frg::manual_box<MemoryReclaimer> globalReclaimer;

static initgraph::Task initReclaim{&globalInitEngine, "generic.init-reclaim",
	initgraph::Requires{getFibersAvailableStage()},
	[] {
		globalReclaimer.initialize();
		globalReclaimer->runReclaimFiber();
	}
};

// --------------------------------------------------------
// MemoryView.
// --------------------------------------------------------

void MemoryView::resize(size_t newSize, async::any_receiver<void> receiver) {
	(void)newSize;
	(void)receiver;
	panicLogger() << "MemoryView does not support resize!" << frg::endlog;
}

void MemoryView::fork(async::any_receiver<frg::tuple<Error, smarter::shared_ptr<MemoryView>>> receiver) {
	receiver.set_value({Error::illegalObject, nullptr});
}

bool MemoryView::asyncLockRange(uintptr_t offset, size_t size,
		smarter::shared_ptr<WorkQueue>, LockRangeNode *node) {
	node->result = lockRange(offset, size);
	return true;
}

Error MemoryView::updateRange(ManageRequest, size_t, size_t) {
	return Error::illegalObject;
}

void MemoryView::submitManage(ManageNode *) {
	panicLogger() << "MemoryView does not support management!" << frg::endlog;
}

void MemoryView::submitInitiateLoad(MonitorNode *initiate) {
	initiate->setup(Error::success);
	initiate->complete();
}

Error MemoryView::setIndirection(size_t, smarter::shared_ptr<MemoryView>,
		uintptr_t, size_t) {
	return Error::illegalObject;
}

// --------------------------------------------------------
// ImmediateMemory
// --------------------------------------------------------

ImmediateMemory::ImmediateMemory(size_t length)
: _physicalPages{*kernelAlloc} {
	auto numPages = (length + kPageSize - 1) >> kPageShift;
	_physicalPages.resize(numPages);
	for(size_t i = 0; i < numPages; ++i) {
		auto physical = physicalAllocator->allocate(kPageSize, 64);
		assert(physical != PhysicalAddr(-1) && "OOM when allocating ImmediateMemory");

		PageAccessor accessor{physical};
		memset(accessor.get(), 0, kPageSize);

		_physicalPages[i] = physical;
	}
}

ImmediateMemory::~ImmediateMemory() {
	for(size_t i = 0; i < _physicalPages.size(); ++i)
		physicalAllocator->free(_physicalPages[i], kPageSize);
}

void ImmediateMemory::resize(size_t newSize, async::any_receiver<void> receiver) {
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		size_t currentNumPages = _physicalPages.size();
		size_t newNumPages = (newSize + kPageSize - 1) >> kPageShift;
		assert(newNumPages >= currentNumPages);
		_physicalPages.resize(newNumPages);
		for(size_t i = currentNumPages; i < newNumPages; ++i) {
			auto physical = physicalAllocator->allocate(kPageSize, 64);
			assert(physical != PhysicalAddr(-1) && "OOM when allocating ImmediateMemory");

			PageAccessor accessor{physical};
			memset(accessor.get(), 0, kPageSize);

			_physicalPages[i] = physical;
		}
	}

	receiver.set_value();
}

frg::expected<Error, frg::tuple<smarter::shared_ptr<GlobalFutexSpace>, uintptr_t>>
ImmediateMemory::resolveGlobalFutex(uintptr_t offset) {
	smarter::shared_ptr<GlobalFutexSpace> futexSpace{selfPtr.lock()};
	return frg::make_tuple(std::move(futexSpace), offset);
}

Error ImmediateMemory::lockRange(uintptr_t, size_t) {
	return Error::success;
}

void ImmediateMemory::unlockRange(uintptr_t, size_t) {
	// Do nothing.
}

frg::tuple<PhysicalAddr, CachingMode> ImmediateMemory::peekRange(uintptr_t offset) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto index = offset >> kPageShift;
	if(index >= _physicalPages.size())
		return {PhysicalAddr(-1), CachingMode::null};
	return {_physicalPages[index], CachingMode::null};
}

bool ImmediateMemory::fetchRange(uintptr_t offset,
		smarter::shared_ptr<WorkQueue>, FetchNode *node) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto index = offset >> kPageShift;
	auto disp = offset & (kPageSize - 1);
	if(index >= _physicalPages.size()) {
		completeFetch(node, Error::fault,
				PhysicalAddr(-1), kPageSize - disp, CachingMode::null);
	}else{
		completeFetch(node, Error::success,
				_physicalPages[index] + disp, kPageSize - disp, CachingMode::null);
	}
	return true;
}

void ImmediateMemory::markDirty(uintptr_t, size_t) {
	// Do nothing for now.
}

size_t ImmediateMemory::getLength() {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	return _physicalPages.size() * kPageSize;
}

coroutine<frg::expected<Error, PhysicalAddr>> ImmediateMemory::takeGlobalFutex(uintptr_t offset,
		smarter::shared_ptr<WorkQueue>) {
	auto index = offset >> kPageShift;
	if(index >= _physicalPages.size())
		co_return Error::fault;
	co_return _physicalPages[index];
}

void ImmediateMemory::retireGlobalFutex(uintptr_t) {
	// Do nothing.
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

frg::expected<Error, frg::tuple<smarter::shared_ptr<GlobalFutexSpace>, uintptr_t>>
HardwareMemory::resolveGlobalFutex(uintptr_t) {
	return Error::illegalObject;
}

Error HardwareMemory::lockRange(uintptr_t, size_t) {
	// Hardware memory is "always locked".
	return Error::success;
}

void HardwareMemory::unlockRange(uintptr_t, size_t) {
	// Hardware memory is "always locked".
}

frg::tuple<PhysicalAddr, CachingMode> HardwareMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);
	return frg::tuple<PhysicalAddr, CachingMode>{_base + offset, _cacheMode};
}

bool HardwareMemory::fetchRange(uintptr_t offset,
		smarter::shared_ptr<WorkQueue>, FetchNode *node) {
	assert(offset % kPageSize == 0);

	completeFetch(node, Error::success, _base + offset, _length - offset, _cacheMode);
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
		infoLogger() << "\e[31mPhysical allocation of size " << (void *)desiredChunkSize
				<< " rounded up to power of 2\e[39m" << frg::endlog;

	size_t length = (desiredLngth + (_chunkSize - 1)) & ~(_chunkSize - 1);
	if(length != desiredLngth)
		infoLogger() << "\e[31mMemory length " << (void *)desiredLngth
				<< " rounded up to chunk size " << (void *)_chunkSize
				<< "\e[39m" << frg::endlog;

	assert(_chunkSize % kPageSize == 0);
	assert(_chunkAlign % kPageSize == 0);
	assert(_chunkSize % _chunkAlign == 0);
	_physicalChunks.resize(length / _chunkSize, PhysicalAddr(-1));
}

AllocatedMemory::~AllocatedMemory() {
	// TODO: This destructor takes a lock. This is potentially unexpected.
	// Rework this to only schedule the deallocation but not actually perform it?
	if(logUsage)
		infoLogger() << "thor: Releasing AllocatedMemory ("
				<< (physicalAllocator->numUsedPages() * 4) << " KiB in use)" << frg::endlog;
	for(size_t i = 0; i < _physicalChunks.size(); ++i) {
		if(_physicalChunks[i] != PhysicalAddr(-1))
			physicalAllocator->free(_physicalChunks[i], _chunkSize);
	}
	if(logUsage)
		infoLogger() << "thor:     ("
				<< (physicalAllocator->numUsedPages() * 4) << " KiB in use)" << frg::endlog;
}

void AllocatedMemory::resize(size_t newSize, async::any_receiver<void> receiver) {
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		assert(!(newSize % _chunkSize));
		size_t num_chunks = newSize / _chunkSize;
		assert(num_chunks >= _physicalChunks.size());
		_physicalChunks.resize(num_chunks, PhysicalAddr(-1));
	}
	receiver.set_value();
}

frg::expected<Error, frg::tuple<smarter::shared_ptr<GlobalFutexSpace>, uintptr_t>>
AllocatedMemory::resolveGlobalFutex(uintptr_t offset) {
	smarter::shared_ptr<GlobalFutexSpace> futexSpace{selfPtr.lock()};
	return frg::make_tuple(std::move(futexSpace), offset);
}

Error AllocatedMemory::lockRange(uintptr_t, size_t) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
	return Error::success;
}

void AllocatedMemory::unlockRange(uintptr_t, size_t) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
}

frg::tuple<PhysicalAddr, CachingMode> AllocatedMemory::peekRange(uintptr_t offset) {
	assert(offset % kPageSize == 0);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto index = offset / _chunkSize;
	auto disp = offset & (_chunkSize - 1);
	assert(index < _physicalChunks.size());

	if(_physicalChunks[index] == PhysicalAddr(-1))
		return frg::tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
	return frg::tuple<PhysicalAddr, CachingMode>{_physicalChunks[index] + disp,
			CachingMode::null};
}

bool AllocatedMemory::fetchRange(uintptr_t offset,
		smarter::shared_ptr<WorkQueue>, FetchNode *node) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

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
	completeFetch(node, Error::success,
			_physicalChunks[index] + disp, _chunkSize - disp, CachingMode::null);
	return true;
}

void AllocatedMemory::markDirty(uintptr_t, size_t) {
	// Do nothing for now.
}

size_t AllocatedMemory::getLength() {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	return _physicalChunks.size() * _chunkSize;
}

coroutine<frg::expected<Error, PhysicalAddr>> AllocatedMemory::takeGlobalFutex(uintptr_t offset,
		smarter::shared_ptr<WorkQueue> wq) {
	// TODO: This could be optimized further (by avoiding the coroutine call).
	auto fetchResult = co_await MemoryView::fetchRange(offset & ~(kPageSize - 1), wq);
	assert(fetchResult.get<0>() == Error::success);
	auto range = fetchResult.get<1>();
	assert(range.get<0>() != PhysicalAddr(-1));
	co_return range.get<0>();
}

void AllocatedMemory::retireGlobalFutex(uintptr_t) {
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
	ManagedPage *pit;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		size_t index = page->identity;
		pit = pages.find(index);
		assert(pit);
		assert(pit->loadState == kStatePresent);
		assert(!pit->lockCount);
		pit->loadState = kStateEvicting;
		globalReclaimer->removePage(&pit->cachePage);
	}

	async::detach_with_allocator(*kernelAlloc, [] (
			ManagedSpace *self, CachePage *page, ManagedPage *pit,
			ReclaimNode *continuation) -> coroutine<void> {
		co_await self->_evictQueue.evictRange(page->identity << kPageShift, kPageSize);

		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&self->mutex);

		if(pit->loadState != kStateEvicting)
			co_return;
		assert(!pit->lockCount);

		if(logUncaching)
			infoLogger() << "\e[33mEvicting physical page\e[39m" << frg::endlog;
		assert(pit->physical != PhysicalAddr(-1));
		physicalAllocator->free(pit->physical, kPageSize);
		pit->loadState = kStateMissing;
		pit->physical = PhysicalAddr(-1);
		continuation->complete();
	}(this, page, pit, continuation));
	return false;
}

void ManagedSpace::retirePage(CachePage *) {
	// TODO: Take a reference to the CachePage when it is first used.
	//       Take a reference to the ManagedSpace for each CachePage in use (so that it is not
	//       destructed until all CachePages are retired).
}

// Note: Neither offset nor size are necessarily multiples of the page size.
Error ManagedSpace::lockPages(uintptr_t offset, size_t size) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex);
	if((offset + size) / kPageSize > numPages)
		return Error::bufferTooSmall;

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		size_t index = (offset + pg) / kPageSize;
		auto [pit, wasInserted] = pages.find_or_insert(index, this, index);
		assert(pit);
		pit->lockCount++;
		if(pit->lockCount == 1) {
			switch(pit->loadState) {
			case kStatePresent:
				globalReclaimer->removePage(&pit->cachePage); break;
			case kStateEvicting:
				// Stop the eviction to keep the page present.
				pit->loadState = kStatePresent; break;
			default:;
			}
		}
		assert(pit->loadState != kStateEvicting);
	}
	return Error::success;
}

// Note: Neither offset nor size are necessarily multiples of the page size.
void ManagedSpace::unlockPages(uintptr_t offset, size_t size) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex);
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
	ManageList pending;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex);

		_managementQueue.push_back(node);
		_progressManagement(pending);
	}

	while(!pending.empty()) {
		auto node = pending.pop_front();
		node->complete();
	}
}

void ManagedSpace::submitMonitor(MonitorNode *node) {
	node->progress = 0;

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex);

	assert(node->offset % kPageSize == 0);
	assert(node->length % kPageSize == 0);
	assert((node->offset + node->length) / kPageSize <= numPages);

	_monitorQueue.push_back(node);
	_progressMonitors();
}

void ManagedSpace::_progressManagement(ManageList &pending) {
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
		node->setup(Error::success, ManageRequest::writeback,
				index << kPageShift, count << kPageShift);
		pending.push_back(node);
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
		node->setup(Error::success, ManageRequest::initialize,
				index << kPageShift, count << kPageShift);
		pending.push_back(node);
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
			node->setup(Error::success);
			node->complete();
		}
	}
}

// --------------------------------------------------------
// BackingMemory
// --------------------------------------------------------

void BackingMemory::resize(size_t newSize, async::any_receiver<void> receiver) {
	assert(!(newSize & (kPageSize - 1)));
	auto newPages = newSize >> kPageShift;

	async::detach_with_allocator(*kernelAlloc, [] (BackingMemory *self, size_t newPages,
			async::any_receiver<void> receiver) -> coroutine<void> {
		size_t oldPages;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&self->_managed->mutex);

			oldPages = self->_managed->numPages;
			self->_managed->numPages = newPages;
		}

		if(newPages > self->_managed->numPages) {
			// Do nothing for now.
		}else if(newPages < self->_managed->numPages) {
			// TODO: also free the affected pages!
			co_await self->_managed->_evictQueue.evictRange(newPages << kPageShift,
					oldPages << kPageShift);
		}

		receiver.set_value();
	}(this, newPages, std::move(receiver)));
}

frg::expected<Error, frg::tuple<smarter::shared_ptr<GlobalFutexSpace>, uintptr_t>>
BackingMemory::resolveGlobalFutex(uintptr_t) {
	return Error::illegalObject;
}

Error BackingMemory::lockRange(uintptr_t offset, size_t size) {
	return _managed->lockPages(offset, size);
}

void BackingMemory::unlockRange(uintptr_t offset, size_t size) {
	_managed->unlockPages(offset, size);
}

frg::tuple<PhysicalAddr, CachingMode> BackingMemory::peekRange(uintptr_t offset) {
	assert(!(offset % kPageSize));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->numPages);
	auto pit = _managed->pages.find(index);

	if(!pit)
		return frg::tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
	return frg::tuple<PhysicalAddr, CachingMode>{pit->physical, CachingMode::null};
}

bool BackingMemory::fetchRange(uintptr_t offset,
		smarter::shared_ptr<WorkQueue>, FetchNode *node) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_managed->mutex);

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

	completeFetch(node, Error::success, pit->physical + misalign, kPageSize - misalign,
			CachingMode::null);
	return true;
}

void BackingMemory::markDirty(uintptr_t, size_t) {
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

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_managed->mutex);
	assert((offset + length) / kPageSize <= _managed->numPages);

/*	assert(length == kPageSize);
	auto inspect = (unsigned char *)physicalToVirtual(_managed->physicalPages[offset / kPageSize]);
	auto log = infoLogger() << "dump";
	for(size_t b = 0; b < kPageSize; b += 16) {
		log << frg::hex_fmt(offset + b) << "   ";
		for(size_t i = 0; i < 16; i++)
			log << " " << frg::hex_fmt(inspect[b + i]);
		log << "\n";
	}
	log << frg::endlog;*/

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

	return Error::success;
}

// --------------------------------------------------------
// FrontalMemory
// --------------------------------------------------------

frg::expected<Error, frg::tuple<smarter::shared_ptr<GlobalFutexSpace>, uintptr_t>>
FrontalMemory::resolveGlobalFutex(uintptr_t offset) {
	smarter::shared_ptr<GlobalFutexSpace> futexSpace{selfPtr.lock()};
	return frg::make_tuple(std::move(futexSpace), offset);
}

Error FrontalMemory::lockRange(uintptr_t offset, size_t size) {
	return _managed->lockPages(offset, size);
}

void FrontalMemory::unlockRange(uintptr_t offset, size_t size) {
	_managed->unlockPages(offset, size);
}

frg::tuple<PhysicalAddr, CachingMode> FrontalMemory::peekRange(uintptr_t offset) {
	assert(!(offset % kPageSize));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->numPages);
	auto pit = _managed->pages.find(index);

	if(!pit || pit->loadState != ManagedSpace::kStatePresent)
		return frg::tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
	return frg::tuple<PhysicalAddr, CachingMode>{pit->physical, CachingMode::null};
}

bool FrontalMemory::fetchRange(uintptr_t offset,
		smarter::shared_ptr<WorkQueue> wq, FetchNode *node) {
	ManageList pending;
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		auto index = offset >> kPageShift;
		auto misalign = offset & (kPageSize - 1);
		assert(index < _managed->numPages);

		// Try the fast-paths first.
		auto [pit, wasInserted] = _managed->pages.find_or_insert(index, _managed.get(), index);
		assert(pit);
		switch(pit->loadState) {
		case ManagedSpace::kStatePresent:
		case ManagedSpace::kStateWantWriteback:
		case ManagedSpace::kStateWriteback:
		case ManagedSpace::kStateAnotherWriteback:
		case ManagedSpace::kStateEvicting: {
			auto physical = pit->physical;
			assert(physical != PhysicalAddr(-1));

			switch(pit->loadState) {
			case ManagedSpace::kStatePresent:
				if(!pit->lockCount)
					globalReclaimer->bumpPage(&pit->cachePage);
				break;
			case ManagedSpace::kStateEvicting:
				// Cancel evication -- the page is still needed.
				pit->loadState = ManagedSpace::kStatePresent;
				globalReclaimer->addPage(&pit->cachePage);
				break;
			default:;
			}

			completeFetch(node, Error::success,
					physical + misalign, kPageSize - misalign, CachingMode::null);
			return true;
		}
		case ManagedSpace::kStateMissing:
		case ManagedSpace::kStateWantInitialization:
		case ManagedSpace::kStateInitialization: {
			break;
		}
		default: assert(!"Unexpected load state");
		}

		if(node->flags() & FetchNode::disallowBacking) {
			infoLogger() << "\e[31m" "thor: Backing of page is disallowed" "\e[39m"
					<< frg::endlog;
			completeFetch(node, Error::fault);
			return true;
		}

		// We have to take the slow-path, i.e., perform the fetch asynchronously.
		if(pit->loadState == ManagedSpace::kStateMissing) {
			pit->loadState = ManagedSpace::kStateWantInitialization;
			_managed->_initializationList.push_back(&pit->cachePage);
		}
		_managed->_progressManagement(pending);

		// TODO: Do not allocate memory here; use pre-allocated nodes instead.
		struct Closure {
			uintptr_t offset;
			ManagedSpace::ManagedPage *page;
			FetchNode *fetch;
			ManagedSpace *bundle;

			Worklet worklet;
			MonitorNode initiate;
		} *closure = frg::construct<Closure>(*kernelAlloc);

		struct Ops {
			static void initiated(Worklet *worklet) {
				auto closure = frg::container_of(worklet, &Closure::worklet);
				assert(closure->initiate.error() == Error::success);

				auto irq_lock = frg::guard(&irqMutex());
				auto lock = frg::guard(&closure->bundle->mutex);

				auto misalign = closure->offset & (kPageSize - 1);
				assert(closure->page->loadState == ManagedSpace::kStatePresent);
				auto physical = closure->page->physical;
				assert(physical != PhysicalAddr(-1));

				lock.unlock();
				irq_lock.unlock();

				completeFetch(closure->fetch, Error::success, physical + misalign, kPageSize - misalign,
						CachingMode::null);
				callbackFetch(closure->fetch);
				frg::destruct(*kernelAlloc, closure);
			}
		};

		closure->offset = offset;
		closure->page = pit;
		closure->fetch = node;
		closure->bundle = _managed.get();

		closure->worklet.setup(&Ops::initiated, wq.get());
		closure->initiate.setup(ManageRequest::initialize,
				offset, kPageSize, &closure->worklet);
		closure->initiate.progress = 0;
		_managed->_monitorQueue.push_back(&closure->initiate);
		_managed->_progressMonitors();
	}

	while(!pending.empty()) {
		auto node = pending.pop_front();
		node->complete();
	}

	return false;
}

void FrontalMemory::markDirty(uintptr_t offset, size_t size) {
	assert(!(offset % kPageSize));
	assert(!(size % kPageSize));

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		// Put the pages into the dirty state.
		for(size_t pg = 0; pg < size; pg += kPageSize) {
			auto index = (offset + pg) >> kPageShift;
			auto pit = _managed->pages.find(index);
			assert(pit);
			switch(pit->loadState) {
			case ManagedSpace::kStatePresent:
				pit->loadState = ManagedSpace::kStateWantWriteback;
				if(!pit->lockCount)
					globalReclaimer->removePage(&pit->cachePage);
				_managed->_writebackList.push_back(&pit->cachePage);
				break;
			case ManagedSpace::kStateEvicting:
				pit->loadState = ManagedSpace::kStateWantWriteback;
				assert(!pit->lockCount);
				_managed->_writebackList.push_back(&pit->cachePage);
				break;
			case ManagedSpace::kStateWriteback:
				pit->loadState = ManagedSpace::kStateAnotherWriteback;
				break;
			case ManagedSpace::kStateWantWriteback:
			case ManagedSpace::kStateAnotherWriteback:
				break;
			default:
				assert(!"Unexpected load state");
			}
		}
	}

	// We cannot call management callbacks with locks held, but markDirty() may be called
	// with external locks held; do it on a WorkQueue.
	_managed->_deferredManagement.invoke();
}

size_t FrontalMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->numPages << kPageShift;
}

void FrontalMemory::submitInitiateLoad(MonitorNode *node) {
	ManageList pending;
	{
		// TODO: This assumes that we want to load the range (which might not be true).
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

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

		_managed->_progressManagement(pending);
	}

	_managed->submitMonitor(node);

	while(!pending.empty()) {
		auto node = pending.pop_front();
		node->complete();
	}
}

coroutine<frg::expected<Error, PhysicalAddr>> FrontalMemory::takeGlobalFutex(uintptr_t offset,
		smarter::shared_ptr<WorkQueue> wq) {
	// For now, we pick the trival implementation here.
	auto lockError = co_await MemoryView::asyncLockRange(offset & ~(kPageSize - 1), kPageSize, wq);
	if(lockError != Error::success)
		co_return Error::fault;
	auto fetchResult = co_await MemoryView::fetchRange(offset & ~(kPageSize - 1), wq);
	if(fetchResult.get<0>() != Error::success)
		co_return Error::fault;
	auto range = fetchResult.get<1>();
	assert(range.get<0>() != PhysicalAddr(-1));
	co_return range.get<0>();
}

void FrontalMemory::retireGlobalFutex(uintptr_t offset) {
	unlockRange(offset & ~(kPageSize - 1), kPageSize);
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

frg::expected<Error, frg::tuple<smarter::shared_ptr<GlobalFutexSpace>, uintptr_t>>
IndirectMemory::resolveGlobalFutex(uintptr_t offset) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);
	if(slot >= indirections_.size())
		return Error::fault;
	if(!indirections_[slot])
		return Error::fault;
	return indirections_[slot]->memory->resolveGlobalFutex(inSlotOffset);
}

Error IndirectMemory::lockRange(uintptr_t offset, size_t size) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);
	if(slot >= indirections_.size())
		return Error::fault;
	if(!indirections_[slot])
		return Error::fault;
	if(inSlotOffset + size > indirections_[slot]->size)
		return Error::fault;
	return indirections_[slot]->memory->lockRange(indirections_[slot]->offset
			+ inSlotOffset, size);
}

void IndirectMemory::unlockRange(uintptr_t offset, size_t size) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);
	assert(slot < indirections_.size()); // TODO: Return Error::fault.
	assert(indirections_[slot]); // TODO: Return Error::fault.
	assert(inSlotOffset + size <= indirections_[slot]->size); // TODO: Return Error::fault.
	return indirections_[slot]->memory->unlockRange(indirections_[slot]->offset
			+ inSlotOffset, size);
}

frg::tuple<PhysicalAddr, CachingMode> IndirectMemory::peekRange(uintptr_t offset) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);
	assert(slot < indirections_.size()); // TODO: Return Error::fault.
	assert(indirections_[slot]); // TODO: Return Error::fault.
	return indirections_[slot]->memory->peekRange(indirections_[slot]->offset
			+ inSlotOffset);
}

bool IndirectMemory::fetchRange(uintptr_t offset,
		smarter::shared_ptr<WorkQueue> wq, FetchNode *node) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);
	assert(slot < indirections_.size()); // TODO: Return Error::fault.
	assert(indirections_[slot]); // TODO: Return Error::fault.
	return indirections_[slot]->memory->fetchRange(indirections_[slot]->offset
			+ inSlotOffset, std::move(wq), node);
}

void IndirectMemory::markDirty(uintptr_t offset, size_t size) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);
	assert(slot < indirections_.size()); // TODO: Return Error::fault.
	assert(indirections_[slot]); // TODO: Return Error::fault.
	assert(inSlotOffset + size <= indirections_[slot]->size); // TODO: Return Error::fault.
	indirections_[slot]->memory->markDirty(indirections_[slot]->offset
			+ inSlotOffset, size);
}

size_t IndirectMemory::getLength() {
	return indirections_.size() << 32;
}

Error IndirectMemory::setIndirection(size_t slot, smarter::shared_ptr<MemoryView> memory,
		uintptr_t offset, size_t size) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	if(slot >= indirections_.size())
		return Error::outOfBounds;
	auto indirection = smarter::allocate_shared<IndirectionSlot>(*kernelAlloc,
			this, slot, memory, offset, size);
	// TODO: start a coroutine to observe evictions.
	memory->addObserver(&indirection->observer);
	indirections_[slot] = std::move(indirection);
	return Error::success;
}

// --------------------------------------------------------
// CopyOnWriteMemory
// --------------------------------------------------------

CopyOnWriteMemory::CopyOnWriteMemory(smarter::shared_ptr<MemoryView> view,
		uintptr_t offset, size_t length,
		smarter::shared_ptr<CowChain> chain)
: MemoryView{&_evictQueue}, _view{std::move(view)},
		_viewOffset{offset}, _length{length}, _copyChain{std::move(chain)},
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

frg::expected<Error, frg::tuple<smarter::shared_ptr<GlobalFutexSpace>, uintptr_t>>
CopyOnWriteMemory::resolveGlobalFutex(uintptr_t offset) {
	smarter::shared_ptr<GlobalFutexSpace> futexSpace{selfPtr.lock()};
	return frg::make_tuple(std::move(futexSpace), offset);
}

void CopyOnWriteMemory::fork(async::any_receiver<frg::tuple<Error, smarter::shared_ptr<MemoryView>>> receiver) {
	// Note that locked pages require special attention during CoW: as we cannot
	// replace them by copies, we have to copy them eagerly.
	// Therefore, they are special-cased below.
	smarter::shared_ptr<CopyOnWriteMemory> forked;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		// Create a new CowChain for both the original and the forked mapping.
		// To correct handle locks pages, we move only non-locked pages from
		// the original mapping to the new chain.
		auto newChain = smarter::allocate_shared<CowChain>(*kernelAlloc, _copyChain);

		// Update the original mapping
		_copyChain = newChain;

		// Create a new mapping in the forked space.
		forked = smarter::allocate_shared<CopyOnWriteMemory>(*kernelAlloc,
				_view, _viewOffset, _length, newChain);
		forked->selfPtr = forked;

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
	}

	async::detach_with_allocator(*kernelAlloc,
			[] (CopyOnWriteMemory *self, smarter::shared_ptr<CopyOnWriteMemory> forked,
			async::any_receiver<frg::tuple<Error, smarter::shared_ptr<MemoryView>>> receiver)
			-> coroutine<void> {
		co_await self->_evictQueue.evictRange(0, self->_length);
		receiver.set_value({Error::success, std::move(forked)});
	}(this, std::move(forked), receiver));
}

Error CopyOnWriteMemory::lockRange(uintptr_t, size_t) {
	panicLogger() << "CopyOnWriteMemory does not support synchronous lockRange()"
			<< frg::endlog;
	__builtin_unreachable();
}

bool CopyOnWriteMemory::asyncLockRange(uintptr_t offset, size_t size,
		smarter::shared_ptr<WorkQueue> wq, LockRangeNode *node) {
	// For now, it is enough to populate the range, as pages can only be evicted from
	// the root of the CoW chain, but copies are never evicted.
	async::detach_with_allocator(*kernelAlloc, [] (CopyOnWriteMemory *self, uintptr_t overallOffset, size_t size,
			smarter::shared_ptr<WorkQueue> wq, LockRangeNode *node) -> coroutine<void> {
		size_t progress = 0;
		while(progress < size) {
			auto offset = overallOffset + progress;

			smarter::shared_ptr<CowChain> chain;
			smarter::shared_ptr<MemoryView> view;
			uintptr_t viewOffset;
			CowPage *cowIt;
			bool waitForCopy = false;
			{
				// If the page is present in our private chain, we just return it.
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->_mutex);

				cowIt = self->_ownedPages.find(offset >> kPageShift);
				if(cowIt) {
					if(cowIt->state == CowState::hasCopy) {
						assert(cowIt->physical != PhysicalAddr(-1));

						cowIt->lockCount++;
						progress += kPageSize;
						continue;
					}else{
						assert(cowIt->state == CowState::inProgress);
						waitForCopy = true;
					}
				}else{
					chain = self->_copyChain;
					view = self->_view;
					viewOffset = self->_viewOffset;

					// Otherwise we need to copy from the chain or from the root view.
					cowIt = self->_ownedPages.insert(offset >> kPageShift);
					cowIt->state = CowState::inProgress;
				}
			}

			if(waitForCopy) {
				bool stillWaiting;
				do {
					stillWaiting = co_await self->_copyEvent.async_wait_if([&] () -> bool {
						// TODO: this could be faster if cowIt->state was atomic.
						auto irqLock = frg::guard(&irqMutex());
						auto lock = frg::guard(&self->_mutex);

						if(cowIt->state == CowState::inProgress)
							return true;
						assert(cowIt->state == CowState::hasCopy);
						return false;
					});
					co_await wq->schedule();
				} while(stillWaiting);

				{
					auto irqLock = frg::guard(&irqMutex());
					auto lock = frg::guard(&self->_mutex);

					assert(cowIt->state == CowState::hasCopy);
					cowIt->lockCount++;
				}
				progress += kPageSize;
				continue;
			}

			PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
			assert(physical != PhysicalAddr(-1) && "OOM");
			PageAccessor accessor{physical};

			// Try to copy from a descendant CoW chain.
			auto pageOffset = viewOffset + offset;
			while(chain) {
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&chain->_mutex);

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
						accessor.get(), kPageSize, wq);
			}

			// To make CoW unobservable, we first need to evict the page here.
			// TODO: enable read-only eviction.
			co_await self->_evictQueue.evictRange(offset & ~(kPageSize - 1), kPageSize);

			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->_mutex);

				assert(cowIt->state == CowState::inProgress);
				cowIt->state = CowState::hasCopy;
				cowIt->physical = physical;
				cowIt->lockCount++;
			}
			self->_copyEvent.raise();
			progress += kPageSize;
		}

		node->result = Error::success;
		node->resume();
	}(this, offset, size, std::move(wq), node));
	return false;
}

void CopyOnWriteMemory::unlockRange(uintptr_t offset, size_t size) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto it = _ownedPages.find((offset + pg) >> kPageShift);
		assert(it);
		assert(it->state == CowState::hasCopy);
		assert(it->lockCount > 0);
		it->lockCount--;
	}
}

frg::tuple<PhysicalAddr, CachingMode> CopyOnWriteMemory::peekRange(uintptr_t offset) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	if(auto it = _ownedPages.find(offset >> kPageShift); it) {
		assert(it->state == CowState::hasCopy);
		return frg::tuple<PhysicalAddr, CachingMode>{it->physical, CachingMode::null};
	}

	return frg::tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
}

bool CopyOnWriteMemory::fetchRange(uintptr_t offset,
		smarter::shared_ptr<WorkQueue> wq, FetchNode *node) {
	async::detach_with_allocator(*kernelAlloc, [] (CopyOnWriteMemory *self, uintptr_t offset,
			smarter::shared_ptr<WorkQueue> wq, FetchNode *node) -> coroutine<void> {
		smarter::shared_ptr<CowChain> chain;
		smarter::shared_ptr<MemoryView> view;
		uintptr_t viewOffset;
		CowPage *cowIt;
		bool waitForCopy = false;
		{
			// If the page is present in our private chain, we just return it.
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&self->_mutex);

			cowIt = self->_ownedPages.find(offset >> kPageShift);
			if(cowIt) {
				if(cowIt->state == CowState::hasCopy) {
					assert(cowIt->physical != PhysicalAddr(-1));

					completeFetch(node, Error::success, cowIt->physical, kPageSize, CachingMode::null);
					callbackFetch(node);
					co_return;
				}else{
					assert(cowIt->state == CowState::inProgress);
					waitForCopy = true;
				}
			}else{
				chain = self->_copyChain;
				view = self->_view;
				viewOffset = self->_viewOffset;

				// Otherwise we need to copy from the chain or from the root view.
				cowIt = self->_ownedPages.insert(offset >> kPageShift);
				cowIt->state = CowState::inProgress;
			}
		}

		if(waitForCopy) {
			bool stillWaiting;
			do {
				stillWaiting = co_await self->_copyEvent.async_wait_if([&] () -> bool {
					// TODO: this could be faster if cowIt->state was atomic.
					auto irqLock = frg::guard(&irqMutex());
					auto lock = frg::guard(&self->_mutex);

					if(cowIt->state == CowState::inProgress)
						return true;
					assert(cowIt->state == CowState::hasCopy);
					return false;
				});
				co_await wq->schedule();
			} while(stillWaiting);

			completeFetch(node, Error::success, cowIt->physical, kPageSize, CachingMode::null);
			callbackFetch(node);
			co_return;
		}

		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		assert(physical != PhysicalAddr(-1) && "OOM");
		PageAccessor accessor{physical};

		// Try to copy from a descendant CoW chain.
		auto pageOffset = viewOffset + offset;
		while(chain) {
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&chain->_mutex);

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
					accessor.get(), kPageSize, wq);
		}

		// To make CoW unobservable, we first need to evict the page here.
		// TODO: enable read-only eviction.
		co_await self->_evictQueue.evictRange(offset, kPageSize);

		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&self->_mutex);

			assert(cowIt->state == CowState::inProgress);
			cowIt->state = CowState::hasCopy;
			cowIt->physical = physical;
		}
		self->_copyEvent.raise();
		completeFetch(node, Error::success, cowIt->physical, kPageSize, CachingMode::null);
		callbackFetch(node);
	}(this, offset, std::move(wq), node));
	return false;
}

void CopyOnWriteMemory::markDirty(uintptr_t, size_t) {
	// We do not need to track dirty pages.
}

coroutine<frg::expected<Error, PhysicalAddr>> CopyOnWriteMemory::takeGlobalFutex(uintptr_t offset,
		smarter::shared_ptr<WorkQueue> wq) {
	// For now, we pick the trival implementation here.
	auto lockError = co_await MemoryView::asyncLockRange(offset & ~(kPageSize - 1), kPageSize, wq);
	if(lockError != Error::success)
		co_return Error::fault;
	auto fetchResult = co_await MemoryView::fetchRange(offset & ~(kPageSize - 1), wq);
	if(fetchResult.get<0>() != Error::success)
		co_return Error::fault;
	auto range = fetchResult.get<1>();
	assert(range.get<0>() != PhysicalAddr(-1));
	co_return range.get<0>();
}

void CopyOnWriteMemory::retireGlobalFutex(uintptr_t offset) {
	unlockRange(offset & ~(kPageSize - 1), kPageSize);
}

// --------------------------------------------------------------------------------------

namespace {
	frg::eternal<FutexRealm> globalFutexRealm;
}

FutexRealm *getGlobalFutexRealm() {
	return &globalFutexRealm.get();
}

} // namespace thor
