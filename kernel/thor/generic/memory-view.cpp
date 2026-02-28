#include <frg/scope_exit.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch-generic/asid.hpp>
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

	// The following flags are debugging options to debug the correctness of various components.
	constexpr bool tortureUncaching = false;
	constexpr bool disableUncaching = false;
}

// --------------------------------------------------------
// Reclaim implementation.
// --------------------------------------------------------

struct MemoryReclaimer {
	void addPage(CachePage *page) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		assert(!(page->flags & CachePage::reclaimRegistered));

		_lruList.push_back(page);
		page->flags |= CachePage::reclaimRegistered;
		_cachedSize += kPageSize;
	}

	void removePage(CachePage *page) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		assert(page->flags & CachePage::reclaimRegistered);

		if(page->flags & CachePage::reclaimPosted) {
			if(!(page->flags & CachePage::reclaimInflight)) {
				auto it = page->bundle->_reclaimList.iterator_to(page);
				page->bundle->_reclaimList.erase(it);
			}

			page->flags &= ~(CachePage::reclaimPosted | CachePage::reclaimInflight);
		}else{
			auto it = _lruList.iterator_to(page);
			_lruList.erase(it);
			_cachedSize -= kPageSize;
		}
		page->flags &= ~CachePage::reclaimRegistered;
	}

	void bumpPage(CachePage *page) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		assert(page->flags & CachePage::reclaimRegistered);

		if(page->flags & CachePage::reclaimPosted) {
			if(!(page->flags & CachePage::reclaimInflight)) {
				auto it = page->bundle->_reclaimList.iterator_to(page);
				page->bundle->_reclaimList.erase(it);
			}

			page->flags &= ~(CachePage::reclaimPosted | CachePage::reclaimInflight);
			_cachedSize += kPageSize;
		}else{
			auto it = _lruList.iterator_to(page);
			_lruList.erase(it);
		}

		_lruList.push_back(page);
	}

	auto awaitReclaim(CacheBundle *bundle, async::cancellation_token ct = {}) {
		return async::transform(
			bundle->_reclaimEvent.async_wait(ct),
			[] (auto) { }
		);
	}

	CachePage *reclaimPage(CacheBundle *bundle) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		if(bundle->_reclaimList.empty())
			return nullptr;

		auto page = bundle->_reclaimList.pop_front();

		assert(page->flags & CachePage::reclaimRegistered);
		assert(page->flags & CachePage::reclaimPosted);
		assert(!(page->flags & CachePage::reclaimInflight));

		page->flags |= CachePage::reclaimInflight;

		return page;
	}

	void runReclaimFiber() {
		auto checkReclaim = [this] () -> bool {
			if(disableUncaching)
				return false;

			auto irqLock = frg::guard(&irqMutex());
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

			auto page = _lruList.pop_front();

			assert(page->flags & CachePage::reclaimRegistered);
			assert(!(page->flags & CachePage::reclaimPosted));
			assert(!(page->flags & CachePage::reclaimInflight));

			page->flags |= CachePage::reclaimPosted;
			_cachedSize -= kPageSize;

			page->bundle->_reclaimList.push_back(page);
			page->bundle->_reclaimEvent.raise();

			return true;
		};

		KernelFiber::run([=, this] {
			while(true) {
				if(logUncaching) {
					auto irqLock = frg::guard(&irqMutex());
					auto lock = frg::guard(&_mutex);
					infoLogger() << "thor: " << (_cachedSize / 1024)
							<< " KiB of cached pages" << frg::endlog;
				}

				while(checkReclaim())
					;
				if(tortureUncaching) {
					KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(10'000'000));
				}else{
					KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(1'000'000'000));
				}
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

coroutine<frg::expected<Error>> MemoryView::resize(size_t newSize) {
	assert(currentIpl() == ipl::exceptionalWork);
	(void)newSize;
	co_return Error::illegalObject;
}

coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> MemoryView::fork() {
	assert(currentIpl() == ipl::exceptionalWork);
	co_return Error::illegalObject;
}

coroutine<frg::expected<Error>> MemoryView::copyTo(uintptr_t offset,
		const void *pointer, size_t size,
		FetchFlags flags) {
	// fetchRequireMutable is managed by this function.
	assert(!(flags & fetchRequireMutable));
	assert(currentIpl() == ipl::exceptionalWork);

	if (auto err = lockRange(offset, size); err != Error::success)
		co_return err;
	frg::scope_exit unlockOnExit{[&] {
		unlockRange(offset, size);
	}};

	size_t progress = 0;
	while(progress < size) {
		auto fetchOffset = (offset + progress) & ~(kPageSize - 1);
		FRG_CO_TRY(co_await touchRange(fetchOffset, kPageSize, flags | fetchRequireMutable));
		auto range = peekRange(fetchOffset, flags | fetchRequireMutable);
		assert(range.physical != PhysicalAddr(-1));
		assert(range.isMutable);

		auto misalign = (offset + progress) & (kPageSize - 1);
		size_t chunk = frg::min(kPageSize - misalign, size - progress);

		PageAccessor accessor{range.physical};
		memcpy(
			reinterpret_cast<uint8_t *>(accessor.get()) + misalign,
			reinterpret_cast<const uint8_t *>(pointer) + progress,
			chunk
		);
		progress += chunk;
	}

	// In addition to what copyFrom() does, we also have to mark the memory as dirty.
	auto misalign = offset & (kPageSize - 1);
	markDirty(
		offset & ~(kPageSize - 1),
		(size + misalign + kPageSize - 1) & ~(kPageSize - 1)
	);

	co_return {};
}

coroutine<frg::expected<Error>> MemoryView::copyFrom(uintptr_t offset,
		void *pointer, size_t size,
		FetchFlags flags) {
	// fetchRequireMutable is managed by this function.
	assert(!(flags & fetchRequireMutable));
	assert(currentIpl() == ipl::exceptionalWork);

	if (auto err = lockRange(offset, size); err != Error::success)
		co_return err;
	frg::scope_exit unlockOnExit{[&] {
		unlockRange(offset, size);
	}};

	size_t progress = 0;
	while(progress < size) {
		auto fetchOffset = (offset + progress) & ~(kPageSize - 1);
		FRG_CO_TRY(co_await touchRange(fetchOffset, kPageSize, flags));
		auto range = peekRange(fetchOffset, flags);
		assert(range.physical != PhysicalAddr(-1));

		auto misalign = (offset + progress) & (kPageSize - 1);
		size_t chunk = frg::min(kPageSize - misalign, size - progress);

		PageAccessor accessor{range.physical};
		memcpy(
			reinterpret_cast<uint8_t *>(pointer) + progress,
			reinterpret_cast<uint8_t *>(accessor.get()) + misalign,
			chunk
		);
		progress += chunk;
	}

	co_return {};
}

coroutine<frg::expected<Error>>
MemoryView::touchFullRange(uintptr_t offset, size_t size, FetchFlags flags) {
	size_t progress = 0;
	while (progress < size) {
		auto chunk = FRG_CO_TRY(co_await touchRange(offset + progress, size - progress, flags));
		progress += chunk;
	}
	co_return {};
}

Error MemoryView::updateRange(ManageRequest, size_t, size_t) {
	return Error::illegalObject;
}

coroutine<frg::expected<Error>> MemoryView::writebackFence(uintptr_t, size_t) {
	co_return {};
}

coroutine<frg::expected<Error>> MemoryView::invalidateRange(uintptr_t, size_t) {
	co_return {};
}

coroutine<frg::expected<Error, MemoryNotification>> MemoryView::pollNotification() {
	co_return Error::illegalObject;
}

Error MemoryView::setIndirection(size_t, smarter::shared_ptr<MemoryView>,
		uintptr_t, size_t, CachingFlags) {
	return Error::illegalObject;
}

coroutine<frg::expected<Error>> copyBetweenViews(
		MemoryView *destView, uintptr_t destOffset,
		MemoryView *srcView, uintptr_t srcOffset, size_t size) {
	if (auto err = destView->lockRange(destOffset, size); err != Error::success)
		co_return err;
	frg::scope_exit unlockDestOnExit{[&] {
		destView->unlockRange(destOffset, size);
	}};

	if (auto err = srcView->lockRange(srcOffset, size); err != Error::success)
		co_return err;
	frg::scope_exit unlockSrcOnExit{[&] {
		srcView->unlockRange(srcOffset, size);
	}};

	size_t progress = 0;
	while(progress < size) {
		auto destFetchOffset = (destOffset + progress) & ~(kPageSize - 1);
		auto srcFetchOffset = (srcOffset + progress) & ~(kPageSize - 1);

		FRG_CO_TRY(co_await destView->touchRange(destFetchOffset, kPageSize, fetchRequireMutable));
		auto destRange = destView->peekRange(destFetchOffset, fetchRequireMutable);
		assert(destRange.physical != PhysicalAddr(-1));
		assert(destRange.isMutable);

		FRG_CO_TRY(co_await srcView->touchRange(srcFetchOffset, kPageSize, fetchNone));
		auto srcRange = srcView->peekRange(srcFetchOffset, fetchNone);
		assert(srcRange.physical != PhysicalAddr(-1));

		auto destMisalign = (destOffset + progress) & (kPageSize - 1);
		auto srcMisalign = (srcOffset + progress) & (kPageSize - 1);
		size_t chunk = frg::min(
			frg::min(kPageSize - destMisalign,
			kPageSize - srcMisalign), size - progress
		);

		PageAccessor destAccessor{destRange.physical};
		PageAccessor srcAccessor{srcRange.physical};
		memcpy(
			(uint8_t *)destAccessor.get() + destMisalign,
			(uint8_t *)srcAccessor.get() + srcMisalign,
			chunk
		);
		progress += chunk;
	}

	// In addition to what copyFromView() does, we also have to mark the memory as dirty.
	auto misalign = destOffset & (kPageSize - 1);
	destView->markDirty(
		destOffset & ~(kPageSize - 1),
		(size + misalign + kPageSize - 1) & ~(kPageSize - 1)
	);

	co_return {};
}

// --------------------------------------------------------
// getZeroMemory()
// --------------------------------------------------------

namespace {

struct ZeroMemory final : MemoryView {
	ZeroMemory() {
		_zeroPage = physicalAllocator->allocate(kPageSize);
		if (_zeroPage == PhysicalAddr(-1))
			panicLogger() << "thor: OOM when trying to allocate zero page" << frg::endlog;
		PageAccessor accessor{_zeroPage};
		memset(accessor.get(), 0, kPageSize);
	}
	ZeroMemory(const ZeroMemory &) = delete;
	~ZeroMemory() = default;

	ZeroMemory &operator= (const ZeroMemory &) = delete;

	size_t getLength() override {
		return size_t{1} << 46;
	}

	coroutine<frg::expected<Error>> copyFrom(uintptr_t, void *buffer, size_t size,
			FetchFlags) override {
		memset(buffer, 0, size);
		co_return {};
	}

	Error lockRange(uintptr_t, size_t) override {
		return Error::success;
	}

	void unlockRange(uintptr_t, size_t) override {
		// Do nothing.
	}

	PhysicalRange peekRange(uintptr_t offset, FetchFlags flags) override {
		assert(!(offset & (kPageSize - 1)));
		if(flags & fetchRequireMutable)
			return PhysicalRange{.physical = PhysicalAddr(-1), .size = 0,
					.cachingMode = CachingMode::null};
		return PhysicalRange{.physical = _zeroPage, .size = kPageSize,
				.cachingMode = CachingMode::null, .isMutable = false};
	}

	coroutine<frg::expected<Error, size_t>>
	touchRange(uintptr_t offset, size_t, FetchFlags flags) override {
		if(flags & fetchRequireMutable)
			co_return Error::badPermissions;
		auto misalign = offset & (kPageSize - 1);
		co_return kPageSize - misalign;
	}

	void markDirty(uintptr_t, size_t) override {
		urgentLogger() << "thor: ZeroMemory::markDirty() called," << frg::endlog;
	}

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<ZeroMemory> selfPtr;

private:
	PhysicalAddr _zeroPage;
};

}

smarter::shared_ptr<MemoryView> getZeroMemory() {
	static frg::eternal<smarter::shared_ptr<ZeroMemory>> singleton = [] {
		auto memory = smarter::allocate_shared<ZeroMemory>(*kernelAlloc);
		memory->selfPtr = memory;
		return memory;
	}();
	return singleton.get();
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

coroutine<frg::expected<Error>> ImmediateMemory::resize(size_t newSize) {
	assert(currentIpl() == ipl::exceptionalWork);

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

	co_return {};
}

Error ImmediateMemory::lockRange(uintptr_t, size_t) {
	return Error::success;
}

void ImmediateMemory::unlockRange(uintptr_t, size_t) {
	// Do nothing.
}

PhysicalRange ImmediateMemory::peekRange(uintptr_t offset, FetchFlags) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto index = offset >> kPageShift;
	if(index >= _physicalPages.size())
		return {.physical = PhysicalAddr(-1), .size = 0, .cachingMode = CachingMode::null};
	return {.physical = _physicalPages[index], .size = kPageSize, .cachingMode = CachingMode::null, .isMutable = true};
}

coroutine<frg::expected<Error, size_t>>
ImmediateMemory::touchRange(uintptr_t offset, size_t, FetchFlags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto index = offset >> kPageShift;
	auto disp = offset & (kPageSize - 1);
	if(index >= _physicalPages.size())
		co_return Error::fault;
	co_return kPageSize - disp;
}

void ImmediateMemory::markDirty(uintptr_t, size_t) {
	// Do nothing for now.
}

size_t ImmediateMemory::getLength() {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	return _physicalPages.size() * kPageSize;
}

// --------------------------------------------------------
// ImmediateWindow
// --------------------------------------------------------

ImmediateWindow::ImmediateWindow(smarter::shared_ptr<ImmediateMemory> memory)
: _memory{std::move(memory)} {
	_size = _memory->getLength();
	_base = KernelVirtualMemory::global().allocate(_size);
	assert(_base);
	for(size_t offset = 0; offset < _size; offset += kPageSize) {
		auto physicalRange = _memory->peekRange(offset, fetchRequireMutable);
		assert(physicalRange.isMutable);
		KernelPageSpace::global().mapSingle4k(
			reinterpret_cast<VirtualAddr>(_base) + offset,
			physicalRange.physical,
			page_access::write,
			CachingMode::null
		);
	}
}

ImmediateWindow::~ImmediateWindow() {
	if(!_base)
		return;
	for(size_t offset = 0; offset < _size; offset += kPageSize)
		KernelPageSpace::global().unmapSingle4k(reinterpret_cast<VirtualAddr>(_base) + offset);
	spawnOnWorkQueue(
		*kernelAlloc,
		WorkQueue::generalQueue().lock(),
		[](smarter::shared_ptr<ImmediateMemory> memory, void *base, size_t size) -> coroutine<void> {
			// The pages are still accessible until shootdown completes, so keep the shared_ptr around.
			(void)memory;
			co_await shootdown(
				&KernelPageSpace::global(),
				reinterpret_cast<VirtualAddr>(base),
				size,
				WorkQueue::generalQueue().get()
			);
			KernelVirtualMemory::global().deallocate(base, size);
		}(std::move(_memory), _base, _size)
	);
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

Error HardwareMemory::lockRange(uintptr_t, size_t) {
	// Hardware memory is "always locked".
	return Error::success;
}

void HardwareMemory::unlockRange(uintptr_t, size_t) {
	// Hardware memory is "always locked".
}

PhysicalRange HardwareMemory::peekRange(uintptr_t offset, FetchFlags) {
	assert(offset % kPageSize == 0);
	return PhysicalRange{.physical = _base + offset, .size = kPageSize, .cachingMode = _cacheMode, .isMutable = true};
}

coroutine<frg::expected<Error, size_t>>
HardwareMemory::touchRange(uintptr_t offset, size_t, FetchFlags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto misalign = offset & (kPageSize - 1);
	co_return kPageSize - misalign;
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
		urgentLogger() << "Physical allocation of size " << (void *)desiredChunkSize
				<< " rounded up to power of 2" << frg::endlog;

	size_t length = (desiredLngth + (_chunkSize - 1)) & ~(_chunkSize - 1);
	if(length != desiredLngth)
		urgentLogger() << "Memory length " << (void *)desiredLngth
				<< " rounded up to chunk size " << (void *)_chunkSize
				<< frg::endlog;

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

coroutine<frg::expected<Error>> AllocatedMemory::resize(size_t newSize) {
	assert(currentIpl() == ipl::exceptionalWork);

	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		assert(!(newSize % _chunkSize));
		size_t num_chunks = newSize / _chunkSize;
		assert(num_chunks >= _physicalChunks.size());
		_physicalChunks.resize(num_chunks, PhysicalAddr(-1));
	}
	co_return {};
}

Error AllocatedMemory::lockRange(uintptr_t, size_t) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
	return Error::success;
}

void AllocatedMemory::unlockRange(uintptr_t, size_t) {
	// For now, we do not evict "anonymous" memory. TODO: Implement eviction here.
}

PhysicalRange AllocatedMemory::peekRange(uintptr_t offset, FetchFlags) {
	assert(offset % kPageSize == 0);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto index = offset / _chunkSize;
	auto disp = offset & (_chunkSize - 1);
	assert(index < _physicalChunks.size());

	if(_physicalChunks[index] == PhysicalAddr(-1))
		return PhysicalRange{.physical = PhysicalAddr(-1), .size = 0, .cachingMode = CachingMode::null};
	return PhysicalRange{.physical = _physicalChunks[index] + disp, .size = kPageSize, .cachingMode = CachingMode::null, .isMutable = true};
}

coroutine<frg::expected<Error, size_t>>
AllocatedMemory::touchRange(uintptr_t offset, size_t, FetchFlags) {
	assert(currentIpl() == ipl::exceptionalWork);

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
	co_return _chunkSize - disp;
}

void AllocatedMemory::markDirty(uintptr_t, size_t) {
	// Do nothing for now.
}

size_t AllocatedMemory::getLength() {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	return _physicalChunks.size() * _chunkSize;
}

// --------------------------------------------------------
// ManagedSpace
// --------------------------------------------------------

ManagedSpace::ManagedSpace(size_t length, bool readahead)
: pages{*kernelAlloc}, numPages{length >> kPageShift}, readahead{readahead} {
	assert(!(length & (kPageSize - 1)));

	[] (ManagedSpace *self, enable_detached_coroutine) -> void {
		while(true) {
			// TODO: Cancel awaitReclaim() when the ManagedSpace is destructed.
			co_await globalReclaimer->awaitReclaim(self);

			CachePage *page;
			ManagedPage *pit;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->mutex);

				page = globalReclaimer->reclaimPage(self);
				if(!page)
					continue;

				size_t index = page->identity;
				pit = self->pages.find(index);
				assert(pit);
				assert(pit->loadState == LoadState::present);
				assert(pit->transactionState == TxState::inReclaim);
				assert(!pit->lockCount);
				pit->loadState = LoadState::evicting;
				pit->transactionState = TxState::none;
				globalReclaimer->removePage(&pit->cachePage);
			}

			co_await self->_evictQueue.evictRange(page->identity << kPageShift, kPageSize);

			PhysicalAddr physical;
			frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> invalidateMonitor;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->mutex);

				if(pit->loadState != LoadState::evicting)
					continue;
				assert(!pit->lockCount);
				assert(pit->physical != PhysicalAddr(-1));
				physical = pit->physical;

				pit->loadState = LoadState::missing;
				pit->transactionState = TxState::none;
				pit->physical = PhysicalAddr(-1);

				if(pit->forceInvalidation) {
					invalidateMonitor = std::move(pit->monitor);
					pit->forceInvalidation = false;
				}
			}

			if(logUncaching)
				warningLogger() << "Evicting physical page" << frg::endlog;
			physicalAllocator->free(physical, kPageSize);
			if(invalidateMonitor)
				invalidateMonitor->event.raise();
		}
	}(this, enable_detached_coroutine{WorkQueue::generalQueue().lock()});
}

ManagedSpace::~ManagedSpace() {
	// TODO: Free all physical memory.
	// TODO: We also have to remove all Loaded/Evicting pages from the reclaimer.
	assert(!"Implement this");
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
			if(pit->loadState == LoadState::present && pit->transactionState == TxState::inReclaim) {
				globalReclaimer->removePage(&pit->cachePage);
				pit->transactionState = TxState::none;
			}else if(pit->loadState == LoadState::evicting) {
				assert(pit->transactionState == TxState::none);
				// Stop the eviction to keep the page present.
				pit->loadState = LoadState::present;
			}
		}
		assert(pit->loadState != LoadState::evicting);
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
			if(pit->loadState == LoadState::present && pit->transactionState == TxState::none) {
				globalReclaimer->addPage(&pit->cachePage);
				pit->transactionState = TxState::inReclaim;
			}
		}
		assert(pit->loadState != LoadState::evicting);
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
		node->completionEvent.raise();
	}
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
			assert(fuse_managed_page->transactionState == TxState::wantWriteback);
			fuse_managed_page->transactionState = TxState::writeback;
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
			assert(fuse_managed_page->transactionState == TxState::wantInitialization);
			fuse_managed_page->transactionState = TxState::initialization;
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


// --------------------------------------------------------
// BackingMemory
// --------------------------------------------------------

coroutine<frg::expected<Error>> BackingMemory::resize(size_t newSize) {
	assert(currentIpl() == ipl::exceptionalWork);
	assert(!(newSize & (kPageSize - 1)));
	auto newPages = newSize >> kPageShift;

	size_t oldPages;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		oldPages = _managed->numPages;
		_managed->numPages = newPages;
	}

	if(newPages > _managed->numPages) {
		// Do nothing for now.
	}else if(newPages < _managed->numPages) {
		// TODO: also free the affected pages!
		co_await _managed->_evictQueue.evictRange(newPages << kPageShift,
				oldPages << kPageShift);
	}

	co_return {};
}

Error BackingMemory::lockRange(uintptr_t offset, size_t size) {
	return _managed->lockPages(offset, size);
}

void BackingMemory::unlockRange(uintptr_t offset, size_t size) {
	_managed->unlockPages(offset, size);
}

PhysicalRange BackingMemory::peekRange(uintptr_t offset, FetchFlags) {
	assert(!(offset % kPageSize));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->numPages);
	auto pit = _managed->pages.find(index);

	if(!pit)
		return PhysicalRange{.physical = PhysicalAddr(-1), .size = 0, .cachingMode = CachingMode::null};
	return PhysicalRange{.physical = pit->physical, .size = kPageSize, .cachingMode = CachingMode::null, .isMutable = true};
}

coroutine<frg::expected<Error, size_t>>
BackingMemory::touchRange(uintptr_t offset, size_t, FetchFlags) {
	assert(currentIpl() == ipl::exceptionalWork);

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

	co_return kPageSize - misalign;
}

void BackingMemory::markDirty(uintptr_t, size_t) {
	// Writes through the BackingMemory do not affect the dirty state!
}

size_t BackingMemory::getLength() {
	// Size is constant so we do not need to lock.
	return _managed->numPages << kPageShift;
}

coroutine<frg::expected<Error, MemoryNotification>> BackingMemory::pollNotification() {
	ManageNode node;
	_managed->submitManagement(&node);
	co_await node.completionEvent.wait();
	if(node.error() != Error::success)
		co_return node.error();
	co_return MemoryNotification{node.type(), node.offset(), node.size()};
}

Error BackingMemory::updateRange(ManageRequest type, size_t offset, size_t length) {
	assert((offset % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	frg::intrusive_list<
		ManagedSpace::TransactionMonitor,
		frg::locate_member<
			ManagedSpace::TransactionMonitor,
			frg::default_list_hook<ManagedSpace::TransactionMonitor>,
			&ManagedSpace::TransactionMonitor::pendingHook
		>
	> pendingMonitors;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		if ((offset + length) / kPageSize > _managed->numPages) {
			infoLogger() << frg::fmt(
				"thor: Requested {} of range {}-{} in BackingMemory of size {}",
				type == ManageRequest::initialize ? "initialization" : "writeback",
				offset, offset + length, _managed->numPages * kPageSize
			) << frg::endlog;

			return Error::illegalArgs;
		}

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
				assert(pit->transactionState == ManagedSpace::TxState::initialization);
				pit->loadState = ManagedSpace::LoadState::present;
				if (pit->lockCount) {
					pit->transactionState = ManagedSpace::TxState::none;
				} else {
					globalReclaimer->addPage(&pit->cachePage);
					pit->transactionState = ManagedSpace::TxState::inReclaim;
				}
				ref_rc(pit->monitor.get());
				pendingMonitors.push_back(pit->monitor.get());
				pit->monitor = {};
			}
		}else{
			for(size_t pg = 0; pg < length; pg += kPageSize) {
				size_t index = (offset + pg) / kPageSize;
				auto pit = _managed->pages.find(index);
				assert(pit);

				assert(pit->transactionState == ManagedSpace::TxState::writeback);
				if(!pit->stillDirty) {
					if (pit->lockCount) {
						pit->transactionState = ManagedSpace::TxState::none;
					} else {
						globalReclaimer->addPage(&pit->cachePage);
						pit->transactionState = ManagedSpace::TxState::inReclaim;
					}
					ref_rc(pit->monitor.get());
					pendingMonitors.push_back(pit->monitor.get());
					pit->monitor = {};
				}else{
					pit->stillDirty = false;
					pit->transactionState = ManagedSpace::TxState::wantWriteback;
					_managed->_writebackList.push_back(&pit->cachePage);
					ref_rc(pit->monitor.get());
					pendingMonitors.push_back(pit->monitor.get());
					// Note that the monitor is destroyed and re-created here.
					pit->monitor = frg::allocate_intrusive_shared<ManagedSpace::TransactionMonitor>(Allocator{});
				}
			}
		}
	}

	while(!pendingMonitors.empty()) {
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> monitor{
			frg::adopt_rc, pendingMonitors.pop_front()
		};
		monitor->event.raise();
	}

	return Error::success;
}

coroutine<frg::expected<Error>> BackingMemory::writebackFence(uintptr_t offset, size_t size) {
	if (offset & (kPageSize - 1))
		co_return Error::illegalArgs;
	if (size & (kPageSize - 1))
		co_return Error::illegalArgs;

	size_t pg = 0;
	while(pg < size) {
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> monitor;
		bool needSecond = false;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_managed->mutex);

			while(pg < size) {
				size_t index = (offset + pg) >> kPageShift;
				auto pit = _managed->pages.find(index);
				if(pit) {
					if(pit->transactionState == ManagedSpace::TxState::wantWriteback) {
						monitor = pit->monitor;
						break;
					} else if(pit->transactionState == ManagedSpace::TxState::writeback) {
						monitor = pit->monitor;
						needSecond = true;
						break;
					}
				}
				pg += kPageSize;
			}
		}

		if(!monitor)
			break;

		co_await monitor->event.wait();

		// If the writeback was already in progress, it is not guaranteed that it did write
		// back the latest state before the writebackFence().
		// In this case, we may need to wait for another writeback.
		if(needSecond) {
			monitor = {};
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_managed->mutex);

				size_t index = (offset + pg) >> kPageShift;
				auto pit = _managed->pages.find(index);
				if(pit && pit->monitor)
					monitor = pit->monitor;
			}

			if(monitor)
				co_await monitor->event.wait();
		}

		pg += kPageSize;
	}

	co_return {};
}

coroutine<frg::expected<Error>> BackingMemory::invalidateRange(uintptr_t offset, size_t size) {
	if (offset & (kPageSize - 1))
		co_return Error::illegalArgs;
	if (size & (kPageSize - 1))
		co_return Error::illegalArgs;

	size_t pg = 0;
	while(pg < size) {
		size_t index = (offset + pg) >> kPageShift;
		frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> monitor;
		bool shouldEvict = false;
		ManagedSpace::ManagedPage *pit = nullptr;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_managed->mutex);

			pit = _managed->pages.find(index);
			if(!pit || pit->loadState == ManagedSpace::LoadState::missing) {
				pg += kPageSize;
				continue;
			}

			if(pit->loadState == ManagedSpace::LoadState::present) {
				if(pit->monitor) {
					monitor = pit->monitor;
				} else if(!pit->lockCount) {
					if(pit->transactionState == ManagedSpace::TxState::inReclaim)
						globalReclaimer->removePage(&pit->cachePage);
					pit->transactionState = ManagedSpace::TxState::none;
					pit->loadState = ManagedSpace::LoadState::evicting;
					shouldEvict = true;
				} else {
					co_return Error::illegalArgs;
				}
			} else {
				assert(pit->loadState == ManagedSpace::LoadState::evicting);
				pit->forceInvalidation = true;
				if(!pit->monitor)
					pit->monitor = frg::allocate_intrusive_shared<ManagedSpace::TransactionMonitor>(Allocator{});
				monitor = pit->monitor;
			}
		}

		if(shouldEvict) {
			co_await _managed->_evictQueue.evictRange(index << kPageShift, kPageSize);
			PhysicalAddr physical;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_managed->mutex);

				if(pit->loadState == ManagedSpace::LoadState::evicting) {
					physical = pit->physical;
					pit->loadState = ManagedSpace::LoadState::missing;
					pit->physical = PhysicalAddr(-1);
				} else {
					physical = PhysicalAddr(-1);
				}
			}
			if(physical != PhysicalAddr(-1))
				physicalAllocator->free(physical, kPageSize);
			if(physical != PhysicalAddr(-1))
				pg += kPageSize;
			continue;
		}

		if(monitor)
			co_await monitor->event.wait();
		else
			pg += kPageSize;
	}

	co_return {};
}

// --------------------------------------------------------
// FrontalMemory
// --------------------------------------------------------

Error FrontalMemory::lockRange(uintptr_t offset, size_t size) {
	return _managed->lockPages(offset, size);
}

void FrontalMemory::unlockRange(uintptr_t offset, size_t size) {
	_managed->unlockPages(offset, size);
}

PhysicalRange FrontalMemory::peekRange(uintptr_t offset, FetchFlags) {
	assert(!(offset % kPageSize));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_managed->mutex);

	auto index = offset / kPageSize;
	assert(index < _managed->numPages);
	auto pit = _managed->pages.find(index);
	if(!pit)
		return PhysicalRange{.physical = PhysicalAddr(-1), .size = 0, .cachingMode = CachingMode::null};

	if(pit->loadState == ManagedSpace::LoadState::present
			|| pit->loadState == ManagedSpace::LoadState::evicting) {
		auto physical = pit->physical;
		assert(physical != PhysicalAddr(-1));

		if(pit->loadState == ManagedSpace::LoadState::evicting) {
			// Cancel evication -- the page is still needed.
			pit->loadState = ManagedSpace::LoadState::present;
			pit->transactionState = ManagedSpace::TxState::inReclaim;
			globalReclaimer->addPage(&pit->cachePage);
		}

		return PhysicalRange{.physical = physical, .size = kPageSize, .cachingMode = CachingMode::null, .isMutable = true};
	}else{
		assert(pit->loadState == ManagedSpace::LoadState::missing);
		return PhysicalRange{.physical = PhysicalAddr(-1), .size = 0, .cachingMode = CachingMode::null};
	}
}

coroutine<frg::expected<Error, size_t>>
FrontalMemory::touchRange(uintptr_t offset, size_t, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto index = offset >> kPageShift;
	auto misalign = offset & (kPageSize - 1);

	ManageList pendingManagement;
	frg::intrusive_shared_ptr<ManagedSpace::TransactionMonitor, Allocator> fetchMonitor;
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_managed->mutex);

		assert(index < _managed->numPages);

		// Try the fast-paths first.
		auto [pit, wasInserted] = _managed->pages.find_or_insert(index, _managed.get(), index);
		assert(pit);
		if(pit->loadState == ManagedSpace::LoadState::present
				|| pit->loadState == ManagedSpace::LoadState::evicting) {
			assert(pit->physical != PhysicalAddr(-1));

			if(pit->transactionState == ManagedSpace::TxState::inReclaim) {
				globalReclaimer->bumpPage(&pit->cachePage);
			}else if(pit->loadState == ManagedSpace::LoadState::evicting) {
				// Cancel evication -- the page is still needed.
				pit->loadState = ManagedSpace::LoadState::present;
				pit->transactionState = ManagedSpace::TxState::inReclaim;
				globalReclaimer->addPage(&pit->cachePage);
			}

			co_return kPageSize - misalign;
		}else{
			assert(pit->loadState == ManagedSpace::LoadState::missing);
		}

		if(flags & fetchDisallowBacking) {
			urgentLogger() << "thor: Backing of page is disallowed" << frg::endlog;
			co_return Error::fault;
		}

		// We have to take the slow-path, i.e., perform the fetch asynchronously.
		if(pit->loadState == ManagedSpace::LoadState::missing
				&& pit->transactionState == ManagedSpace::TxState::none) {
			pit->transactionState = ManagedSpace::TxState::wantInitialization;
			_managed->_initializationList.push_back(&pit->cachePage);
			pit->monitor = frg::allocate_intrusive_shared<ManagedSpace::TransactionMonitor>(Allocator{});
		}

		// Perform readahead.
		if(_managed->readahead)
			for(size_t i = 1; i < 4; ++i) {
				if(!(index + i < _managed->numPages))
					break;
				auto [pit, wasInserted] = _managed->pages.find_or_insert(
						index + i, _managed.get(), index + i);
				assert(pit);
				if(pit->loadState == ManagedSpace::LoadState::missing
						&& pit->transactionState == ManagedSpace::TxState::none) {
					pit->transactionState = ManagedSpace::TxState::wantInitialization;
					_managed->_initializationList.push_back(&pit->cachePage);
					pit->monitor = frg::allocate_intrusive_shared<ManagedSpace::TransactionMonitor>(Allocator{});
				}
			}

		_managed->_progressManagement(pendingManagement);

		fetchMonitor = pit->monitor;
	}

	while(!pendingManagement.empty()) {
		auto node = pendingManagement.pop_front();
		node->completionEvent.raise();
	}

	co_await fetchMonitor->event.wait();

	co_return kPageSize - misalign;
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
			if(pit->loadState == ManagedSpace::LoadState::present
					&& (pit->transactionState == ManagedSpace::TxState::none
						|| pit->transactionState == ManagedSpace::TxState::inReclaim)) {
				if(pit->transactionState == ManagedSpace::TxState::inReclaim)
					globalReclaimer->removePage(&pit->cachePage);
				pit->transactionState = ManagedSpace::TxState::wantWriteback;
				_managed->_writebackList.push_back(&pit->cachePage);
				pit->monitor = frg::allocate_intrusive_shared<ManagedSpace::TransactionMonitor>(Allocator{});
			}else if(pit->loadState == ManagedSpace::LoadState::evicting) {
				pit->loadState = ManagedSpace::LoadState::present;
				pit->transactionState = ManagedSpace::TxState::wantWriteback;
				_managed->_writebackList.push_back(&pit->cachePage);
				pit->monitor = frg::allocate_intrusive_shared<ManagedSpace::TransactionMonitor>(Allocator{});
			}else if(pit->transactionState == ManagedSpace::TxState::writeback) {
				pit->stillDirty = true;
			}else{
				assert(pit->transactionState == ManagedSpace::TxState::wantWriteback);
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

Error IndirectMemory::lockRange(uintptr_t offset, size_t size) {
	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);

	smarter::shared_ptr<IndirectionSlot> indirection;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		if(slot >= indirections_.size())
			return Error::fault;
		if(!indirections_[slot])
			return Error::fault;
		if(inSlotOffset + size > indirections_[slot]->size)
			return Error::fault;
		indirection = indirections_[slot];
	}

	return indirection->memory->lockRange(indirection->offset + inSlotOffset, size);
}

void IndirectMemory::unlockRange(uintptr_t offset, size_t size) {
	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);

	smarter::shared_ptr<IndirectionSlot> indirection;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		// Otherwise, lockRange() would have faulted.
		assert(slot < indirections_.size());
		assert(indirections_[slot]);
		assert(inSlotOffset + size <= indirections_[slot]->size);
		indirection = indirections_[slot];
	}

	indirection->memory->unlockRange(indirection->offset + inSlotOffset, size);
}

PhysicalRange IndirectMemory::peekRange(uintptr_t offset, FetchFlags flags) {
	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);

	smarter::shared_ptr<IndirectionSlot> indirection;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		assert(slot < indirections_.size()); // TODO: Return Error::fault.
		assert(indirections_[slot]); // TODO: Return Error::fault.
		indirection = indirections_[slot];
	}

	auto physicalRange = indirection->memory->peekRange(indirection->offset + inSlotOffset, flags);

	CachingMode cachingMode = CachingMode::null;
	if(indirection->flags & cacheWriteCombine)
		cachingMode = CachingMode::writeCombine;

	return {.physical = physicalRange.physical, .size = physicalRange.size,
		.cachingMode = determineCachingMode(physicalRange.cachingMode, cachingMode),
		.isMutable = physicalRange.isMutable};
}

coroutine<frg::expected<Error, size_t>>
IndirectMemory::touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);

	smarter::shared_ptr<IndirectionSlot> indirection;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		if (slot >= indirections_.size())
			co_return Error::fault;
		if (!indirections_[slot])
			co_return Error::fault;
		if (inSlotOffset >= indirections_[slot]->size)
			co_return Error::fault;

		indirection = indirections_[slot];
	}

	co_return co_await indirection->memory->touchRange(indirection->offset
		+ inSlotOffset, sizeHint, flags);
}

void IndirectMemory::markDirty(uintptr_t offset, size_t size) {
	auto slot = offset >> 32;
	auto inSlotOffset = offset & ((uintptr_t(1) << 32) - 1);

	smarter::shared_ptr<IndirectionSlot> indirection;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		assert(slot < indirections_.size()); // TODO: Return Error::fault.
		assert(indirections_[slot]); // TODO: Return Error::fault.
		assert(inSlotOffset + size <= indirections_[slot]->size); // TODO: Return Error::fault.
		indirection = indirections_[slot];
	}

	indirection->memory->markDirty(indirection->offset + inSlotOffset, size);
}

size_t IndirectMemory::getLength() {
	return indirections_.size() << 32;
}

Error IndirectMemory::setIndirection(size_t slot, smarter::shared_ptr<MemoryView> memory,
		uintptr_t offset, size_t size, CachingFlags flags) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	if(slot >= indirections_.size())
		return Error::outOfBounds;
	auto indirection = smarter::allocate_shared<IndirectionSlot>(*kernelAlloc,
			this, slot, memory, offset, size, flags);
	// TODO: start a coroutine to observe evictions.
	memory->addObserver(&indirection->observer);
	indirections_[slot] = std::move(indirection);
	return Error::success;
}

// --------------------------------------------------------
// CopyOnWriteMemory
// --------------------------------------------------------

CowPage::~CowPage() {
	if(state == CowState::null)
		return;
	assert(state == CowState::hasCopy);
	assert(physical != PhysicalAddr(-1));
	physicalAllocator->free(physical, kPageSize);
}

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
}

size_t CopyOnWriteMemory::getLength() {
	return _length;
}

coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> CopyOnWriteMemory::fork() {
	assert(currentIpl() == ipl::exceptionalWork);

	// Note that locked pages require special attention during CoW: as we cannot
	// replace them by copies, we have to copy them eagerly.
	// Therefore, they are special-cased below.
	smarter::shared_ptr<CopyOnWriteMemory> forked;
	smarter::shared_ptr<CowChain> newChain;
	frg::vector<frg::tuple<size_t, smarter::shared_ptr<CowPage>>, KernelAlloc> inProgressPages{*kernelAlloc};

	auto doCopyOnePage = [&] (size_t pg, smarter::borrowed_ptr<CowPage> page) {
		// The page is locked. We *need* to keep it in the old address space.
		if(page->lockCount /*|| disableCow */) {
			// Allocate a new physical page for a copy.
			auto copyPhysical = physicalAllocator->allocate(kPageSize);
			assert(copyPhysical != PhysicalAddr(-1) && "OOM");

			// As the page is locked anyway, we can just copy it synchronously.
			PageAccessor lockedAccessor{page->physical};
			PageAccessor copyAccessor{copyPhysical};
			memcpy(copyAccessor.get(), lockedAccessor.get(), kPageSize);

			// Update the chains.
			auto copyPage = smarter::allocate_shared<CowPage>(*kernelAlloc);
			copyPage->state = CowState::hasCopy;
			copyPage->physical = copyPhysical;
			auto copyIt = forked->_ownedPages.insert(pg >> kPageShift);
			*copyIt = copyPage;
		}else{
			auto physical = page->physical;
			assert(physical != PhysicalAddr(-1));

			// Update the chains.
			auto pageOffset = _viewOffset + pg;
			auto newIt = newChain->_pages.insert(pageOffset >> kPageShift);
			*newIt = page.lock();
			_ownedPages.erase(pg >> kPageShift);
		}
	};

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		// Create a new CowChain for both the original and the forked mapping.
		// To correct handle locks pages, we move only non-locked pages from
		// the original mapping to the new chain.
		auto curChain = _copyChain;
		newChain = smarter::allocate_shared<CowChain>(*kernelAlloc);

		// Update the original mapping
		_copyChain = newChain;

		// Create a new mapping in the forked space.
		forked = smarter::allocate_shared<CopyOnWriteMemory>(*kernelAlloc,
						_view, _viewOffset, _length, newChain);
		forked->selfPtr = forked;

		// Inspect all copied pages owned by the original mapping.
		for(size_t pg = 0; pg < _length; pg += kPageSize) {
			auto it = _ownedPages.find(pg >> kPageShift);

			if(!it) {
				// If the page is missing in this memory object, look at the CowChain.
				auto pageOffset = _viewOffset + pg;
				if (curChain) {
					auto chainLock = frg::guard(&curChain->_mutex);

					if(auto it = curChain->_pages.find(pageOffset >> kPageShift); it) {
						auto page = *it;
						assert(page->state == CowState::hasCopy);
						auto newIt = newChain->_pages.insert(pageOffset >> kPageShift);
						*newIt = page;
					}
				}
				continue;
			}

			auto page = *it;
			if(page->state == CowState::null) {
				continue;
			}else if(page->state == CowState::inProgress) {
				// We wait for the in progress pages later, as we
				// need to drop the locks we're holding before
				// suspending, but they are ensuring consistency
				// of the object we're working on.
				inProgressPages.push(frg::make_tuple(pg, page));
				continue;
			}else
				assert(page->state == CowState::hasCopy);

			doCopyOnePage(pg, page);
		}
	}

	// Wait for the in progress pages to complete copying.
	bool stillWaiting = inProgressPages.size() > 0;
	while (stillWaiting) {
		stillWaiting = co_await _copyEvent.async_wait_if([&] {
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_mutex);

			for (auto [_, inProgressPage] : inProgressPages) {
				if (inProgressPage->state == CowState::inProgress)
					return true;
			}

			return false;
		});
	}

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		// Copy all the previously in progress pages now that they're done copying.
		for (auto [pg, page] : inProgressPages) {
			assert(page->state == CowState::hasCopy);
			doCopyOnePage(pg, page);
		}
	}

	co_await _evictQueue.evictRange(0, _length);
	co_return smarter::shared_ptr<MemoryView>{std::move(forked)};
}

Error CopyOnWriteMemory::lockRange(uintptr_t offset, size_t size) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto it = _ownedPages.find((offset + pg) >> kPageShift);
		if(it) {
			auto page = *it;
			page->lockCount++;
		}else{
			auto cowPage = smarter::allocate_shared<CowPage>(*kernelAlloc);
			cowPage->lockCount = 1;
			it = _ownedPages.insert((offset + pg) >> kPageShift);
			*it = cowPage;
		}
	}

	return Error::success;
}

void CopyOnWriteMemory::unlockRange(uintptr_t offset, size_t size) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto it = _ownedPages.find((offset + pg) >> kPageShift);
		assert(it);
		auto page = *it;
		assert(page->lockCount > 0);
		page->lockCount--;
	}
}

PhysicalRange CopyOnWriteMemory::peekRange(uintptr_t offset, FetchFlags flags) {
	smarter::shared_ptr<CowChain> chain;
	smarter::shared_ptr<MemoryView> view;
	uintptr_t viewOffset;
	// Note: the passthrough cases here have to match touchRange() since
	//       callers expect touchRange() to make the page available to peekRange().
	bool passthrough = false;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		if(auto it = _ownedPages.find(offset >> kPageShift); it) {
			auto page = *it;
			if(page->state == CowState::hasCopy) {
				assert(page->physical != PhysicalAddr(-1));
				return PhysicalRange{.physical = page->physical, .size = kPageSize, .cachingMode = CachingMode::null, .isMutable = true};
			}
		} else {
			if (!(flags & fetchRequireMutable)) {
				passthrough = true;
			}
		}

		chain = _copyChain;
		view = _view;
		viewOffset = _viewOffset;
	}

	auto pageOffset = viewOffset + offset;
	if (passthrough) {
		if (chain) {
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&chain->_mutex);

			if(auto it = chain->_pages.find(pageOffset >> kPageShift); it) {
				auto page = *it;
				return PhysicalRange{.physical = page->physical, .size = kPageSize, .cachingMode = CachingMode::null, .isMutable = false};
			}
		}

		auto range = view->peekRange(pageOffset, flags);
		// Note: passthrough caching mode etc. but clamp the size to kPageSize.
		if(range.physical != PhysicalAddr(-1))
			return PhysicalRange{.physical = range.physical, .size = kPageSize, .cachingMode = range.cachingMode, .isMutable = false};
	}

	return PhysicalRange{.physical = PhysicalAddr(-1), .size = 0, .cachingMode = CachingMode::null};
}

coroutine<frg::expected<Error, size_t>>
CopyOnWriteMemory::touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) {
	assert(currentIpl() == ipl::exceptionalWork);

	auto misalign = offset & (kPageSize - 1);
	auto alignedOffset = offset & ~(kPageSize - 1);

	smarter::shared_ptr<CowChain> chain;
	smarter::shared_ptr<MemoryView> view;
	uintptr_t viewOffset;
	smarter::shared_ptr<CowPage> cowPage;
	// Note: the passthrough cases here have to match peekRange() since
	//       callers expect touchRange() to make the page available to peekRange().
	bool passthrough = false;
	bool waitForCopy = false;
	{
		// If the page is present in our private chain, we just return it.
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		auto cowIt = _ownedPages.find(offset >> kPageShift);
		if(cowIt) {
			cowPage = *cowIt;
			if(cowPage->state == CowState::hasCopy) {
				assert(cowPage->physical != PhysicalAddr(-1));
				co_return kPageSize - misalign;
			}else if(cowPage->state == CowState::inProgress) {
				waitForCopy = true;
			}else{
				assert(cowPage->state == CowState::null);
				cowPage->state = CowState::inProgress;
			}
		}else{
			if (!(flags & fetchRequireMutable)) {
				passthrough = true;
			} else {
				// Otherwise we need to copy from the chain or from the root view.
				cowPage = smarter::allocate_shared<CowPage>(*kernelAlloc);
				cowPage->state = CowState::inProgress;
				cowIt = _ownedPages.insert(offset >> kPageShift);
				*cowIt = cowPage;
			}
		}

		chain = _copyChain;
		view = _view;
		viewOffset = _viewOffset;
	}

	// Passthrough and waitForCopy are mutually exclusive:
	// if waitForCopy is set, we may need to wait for eviction to finish
	// and we must not return passed through pages after eviction started.
	assert(!(passthrough && waitForCopy));

	auto pageOffset = viewOffset + alignedOffset;
	if(passthrough) {
		if (chain) {
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&chain->_mutex);

			if(chain->_pages.find(pageOffset >> kPageShift))
				co_return kPageSize - misalign;
		}

		co_return co_await view->touchRange(pageOffset, sizeHint, flags);
	}

	if(waitForCopy) {
		bool stillWaiting;
		do {
			stillWaiting = co_await _copyEvent.async_wait_if([&] () -> bool {
				// TODO: this could be faster if cowIt->state was atomic.
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_mutex);

				if(cowPage->state == CowState::inProgress)
					return true;
				assert(cowPage->state == CowState::hasCopy);
				return false;
			});
		} while(stillWaiting);

		co_return kPageSize - misalign;
	}

	PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
	assert(physical != PhysicalAddr(-1) && "OOM");
	PageAccessor accessor{physical};

	// Try to copy from a descendant CoW chain.
	bool chainHasCopy = false;
	if(chain) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&chain->_mutex);

		if(auto it = chain->_pages.find(pageOffset >> kPageShift); it) {
			auto page = *it;
			// We can just copy synchronously here -- the descendant is not evicted.
			assert(page->state == CowState::hasCopy);
			auto srcPhysical = page->physical;
			assert(srcPhysical != PhysicalAddr(-1));
			auto srcAccessor = PageAccessor{srcPhysical};
			memcpy(accessor.get(), srcAccessor.get(), kPageSize);
			chainHasCopy = true;
		}
	}

	// Copy from the root view.
	if(!chainHasCopy) {
		FRG_CO_TRY(co_await view->copyFrom(pageOffset & ~(kPageSize - 1),
				accessor.get(), kPageSize));
	}

	// To make CoW unobservable, we first need to evict the page here.
	co_await _evictQueue.evictRange(alignedOffset, kPageSize);

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		assert(cowPage->state == CowState::inProgress);
		cowPage->state = CowState::hasCopy;
		cowPage->physical = physical;
	}
	_copyEvent.raise();
	co_return kPageSize - misalign;
}

void CopyOnWriteMemory::markDirty(uintptr_t, size_t) {
	// We do not need to track dirty pages.
}

// --------------------------------------------------------------------------------------

namespace {
	frg::eternal<FutexRealm> globalFutexRealm;
}

FutexRealm *getGlobalFutexRealm() {
	return &globalFutexRealm.get();
}

} // namespace thor
