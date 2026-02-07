#pragma once

#include <cstddef>

#include <async/algorithm.hpp>
#include <async/oneshot-event.hpp>
#include <async/post-ack.hpp>
#include <async/recurring-event.hpp>
#include <frg/rcu_radixtree.hpp>
#include <frg/vector.hpp>
#include <frg/expected.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/futex.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/kernel-locks.hpp>

namespace thor {

enum class ManageRequest {
	null,
	initialize,
	writeback
};

struct Mapping;
struct AddressSpace;
struct AddressSpaceLockHandle;
struct FaultNode;
struct MemoryReclaimer;

struct CacheBundle;

struct CachePage {
	// Page is registered with the reclaim mechanism.
	static constexpr uint32_t reclaimRegistered = 0x01;
	// Page is currently being evicted (not in LRU list, but in bundle list).
	static constexpr uint32_t reclaimPosted = 0x02;
	// Page has been evicted (neither in the LRU, nor in the bundle list).
	static constexpr uint32_t reclaimInflight = 0x04;

	// CacheBundle that owns this page.
	CacheBundle *bundle = nullptr;

	// Identity of the page as part of the bundle.
	// Bundles can use this field however they like.
	uint64_t identity = 0;

	// Hooks for LRU lists.
	frg::default_list_hook<CachePage> listHook;

	uint32_t flags = 0;
};

// This is the "backend" part of a memory object.
struct CacheBundle {
	friend struct MemoryReclaimer;

private:
	frg::intrusive_list<
		CachePage,
		frg::locate_member<
			CachePage,
			frg::default_list_hook<CachePage>,
			&CachePage::listHook
		>
	> _reclaimList;

	async::recurring_event _reclaimEvent;
};

using PhysicalRange = frg::tuple<PhysicalAddr, size_t, CachingMode>;

struct ManageNode {
	Error error() { return _error; }
	ManageRequest type() { return _type; }
	uintptr_t offset() { return _offset; }
	size_t size() { return _size; }

	void setup(Error error, ManageRequest type, uintptr_t offset, size_t size) {
		_error = error;
		_type = type;
		_offset = offset;
		_size = size;
	}

	virtual void complete() = 0;

	frg::default_list_hook<ManageNode> processQueueItem;

protected:
	~ManageNode() = default;

private:
	// Results of the operation.
	Error _error;
	ManageRequest _type;
	uintptr_t _offset;
	size_t _size;
};

using ManageList = frg::intrusive_list<
	ManageNode,
	frg::locate_member<
		ManageNode,
		frg::default_list_hook<ManageNode>,
		&ManageNode::processQueueItem
	>
>;

struct MonitorNode {
	void setup(ManageRequest type_, uintptr_t offset_, size_t length_) {
		type = type_;
		offset = offset_;
		length = length_;
	}

	Error error() { return _error; }

	void setup(Error error) {
		_error = error;
	}

	ManageRequest type;
	uintptr_t offset;
	size_t length;
	async::oneshot_event event;

private:
	Error _error;

public:
	frg::default_list_hook<MonitorNode> processQueueItem;

	// Current progress in bytes.
	size_t progress;
};

using MonitorList = frg::intrusive_list<
	MonitorNode,
	frg::locate_member<
		MonitorNode,
		frg::default_list_hook<MonitorNode>,
		&MonitorNode::processQueueItem
	>
>;

using FetchFlags = uint32_t;
inline constexpr FetchFlags fetchDisallowBacking = 1;

using CachingFlags = uint32_t;
inline constexpr CachingFlags cacheWriteCombine = 1;

struct RangeToEvict {
	uintptr_t offset;
	size_t size;
};

struct Eviction {
	Eviction() = default;

	Eviction(async::post_ack_handle<RangeToEvict> handle)
	: handle_{std::move(handle)} { }

	explicit operator bool () {
		return static_cast<bool>(handle_);
	}

	uintptr_t offset() { return handle_->offset; }
	uintptr_t size() { return handle_->size; }

	void done() {
		handle_.ack();
	}

private:
	async::post_ack_handle<RangeToEvict> handle_;
};

struct MemoryObserver {
	friend struct MemoryView;
	friend struct EvictionQueue;

	frg::default_list_hook<MemoryObserver> listHook;

private:
	async::post_ack_agent<RangeToEvict> agent_;
};

struct EvictionQueue {
	void addObserver(MemoryObserver *observer) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		observer->agent_.attach(&mechanism_);
		observers_.push_back(observer);
		numObservers_++;
	}

	void removeObserver(MemoryObserver *observer) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		observer->agent_.detach();
		auto it = observers_.iterator_to(observer);
		observers_.erase(it);
		numObservers_--;
	}

	auto pollEviction(MemoryObserver *observer, async::cancellation_token ct) {
		return observer->agent_.poll(std::move(ct));
	}

	auto evictRange(uintptr_t offset, size_t size) {
		return mechanism_.post(RangeToEvict{offset, size});
	}

private:
	frg::ticket_spinlock mutex_;

	frg::intrusive_list<
		MemoryObserver,
		frg::locate_member<
			MemoryObserver,
			frg::default_list_hook<MemoryObserver>,
			&MemoryObserver::listHook
		>
	> observers_;

	size_t numObservers_ = 0;
	async::post_ack_mechanism<RangeToEvict> mechanism_;
};

// View on some pages of memory. This is the "frontend" part of a memory object.
struct MemoryView {
protected:
	MemoryView(EvictionQueue *associatedEvictionQueue = nullptr)
	: associatedEvictionQueue_{associatedEvictionQueue} { }

	~MemoryView() = default;

public:
	// Add/remove memory observers. These will be notified of page evictions.
	void addObserver(MemoryObserver *observer) {
		if(associatedEvictionQueue_)
			associatedEvictionQueue_->addObserver(observer);
	}

	void removeObserver(MemoryObserver *observer) {
		if(associatedEvictionQueue_)
			associatedEvictionQueue_->removeObserver(observer);
	}

	virtual size_t getLength() = 0;

	virtual coroutine<frg::expected<Error>> resize(size_t newLength);

	virtual coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> fork();

	virtual coroutine<frg::expected<Error>> copyTo(uintptr_t offset,
			const void *pointer, size_t size,
			FetchFlags flags = 0);

	virtual coroutine<frg::expected<Error>> copyFrom(uintptr_t offset,
			void *pointer, size_t size,
			FetchFlags flags = 0);

	// Acquire/release a lock on a memory range.
	// While a lock is active, results of peekRange() stay consistent.
	// Locks do *not* force all pages to be available, but once a page is available
	// (e.g. due to touchRange()), it cannot be evicted until the lock is released.
	virtual Error lockRange(uintptr_t offset, size_t size) = 0;
	virtual void unlockRange(uintptr_t offset, size_t size) = 0;

	// Optimistically returns the physical memory that backs a range of memory.
	// Result stays valid until the range is evicted.
	virtual frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) = 0;

	// Makes a range of memory available for peekRange().
	// The sizeHint parameter is a hint; the implementation may affect fewer bytes.
	// Returns the number of bytes that were actually affected.
	virtual coroutine<frg::expected<Error, size_t>>
	touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags = 0) = 0;

	// Marks a range of pages as dirty.
	virtual void markDirty(uintptr_t offset, size_t size) = 0;

	virtual void submitManage(ManageNode *handle);

	// Called (e.g. by user space) to update a range after loading or writeback.
	virtual Error updateRange(ManageRequest type, size_t offset, size_t length);

	virtual Error setIndirection(size_t slot, smarter::shared_ptr<MemoryView> view,
			uintptr_t offset, size_t size, CachingFlags flags);

	coroutine<frg::expected<Error>>
	touchFullRange(uintptr_t offset, size_t size, FetchFlags flags = 0);

	// ----------------------------------------------------------------------------------
	// Memory eviction.
	// ----------------------------------------------------------------------------------

	bool canEvictMemory() {
		return associatedEvictionQueue_;
	}

	auto pollEviction(MemoryObserver *observer, async::cancellation_token ct) {
		return async::transform(observer->agent_.poll(std::move(ct)),
			[] (async::post_ack_handle<RangeToEvict> handle) {
				return Eviction{std::move(handle)};
			}
		);
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for submitManage()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct SubmitManageOperation;

	struct [[nodiscard]] SubmitManageSender {
		using value_type = frg::tuple<Error, ManageRequest, uintptr_t, size_t>;

		template<typename R>
		friend SubmitManageOperation<R>
		connect(SubmitManageSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		MemoryView *self;
	};

	SubmitManageSender submitManage() {
		return {this};
	}

	template<typename R>
	struct SubmitManageOperation final : private ManageNode {
		SubmitManageOperation(SubmitManageSender s, R receiver)
		: s_{s.self}, receiver_{std::move(receiver)} { }

		SubmitManageOperation(const SubmitManageOperation &) = delete;

		SubmitManageOperation &operator= (const SubmitManageOperation &) = delete;

		void start() {
			s_->submitManage(this);
		}

	private:
		void complete() override {
			async::execution::set_value(receiver_,
					frg::tuple<Error, ManageRequest, uintptr_t, size_t>{error(),
							type(), offset(), size()});
		}

		MemoryView *s_;
		R receiver_;
	};

	friend async::sender_awaiter<SubmitManageSender,
			frg::tuple<Error, ManageRequest, uintptr_t, size_t>>
	operator co_await(SubmitManageSender sender) {
		return {sender};
	}

private:
	EvictionQueue *associatedEvictionQueue_;
};

struct SliceRange {
	MemoryView *view;
	uintptr_t displacement;
	size_t size;
};

struct MemorySlice {
	MemorySlice(smarter::shared_ptr<MemoryView> view,
			ptrdiff_t view_offset, size_t view_size, CachingFlags cachingFlags = 0);

	smarter::shared_ptr<MemoryView> getView() {
		return _view;
	}

	CachingFlags getCachingFlags() const {
		return cachingFlags_;
	}

	uintptr_t offset() { return _viewOffset; }
	size_t length() { return _viewSize; }

private:
	smarter::shared_ptr<MemoryView> _view;
	ptrdiff_t _viewOffset;
	size_t _viewSize;
	CachingFlags cachingFlags_;
};

coroutine<frg::expected<Error>> copyBetweenViews(
		MemoryView *destView, uintptr_t destOffset,
		MemoryView *srcView, uintptr_t srcOffset, size_t size);

// ----------------------------------------------------------------------------------

struct ImmediateMemory;

smarter::shared_ptr<MemoryView> getZeroMemory();

// Memory that is allocated by the kernel and never swapped out.
// In contrast to most other memory objects, it can be accessed synchronously.
struct ImmediateMemory final : MemoryView {
	ImmediateMemory(size_t length);
	ImmediateMemory(const ImmediateMemory &) = delete;
	~ImmediateMemory();

	ImmediateMemory &operator= (const ImmediateMemory &) = delete;

	size_t getLength() override;
	coroutine<frg::expected<Error>> resize(size_t newLength) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	void markDirty(uintptr_t offset, size_t size) override;

	template<typename T>
	T *accessImmediate(uintptr_t offset) {
		static_assert(sizeof(T) <= kPageSize); // Otherwise, the assert below will always fail.
		auto misalign = offset & (kPageSize - 1);
		assert(misalign + sizeof(T) <= kPageSize);

		auto index = offset >> kPageShift;
		assert(index < _physicalPages.size());
		PageAccessor accessor{_physicalPages[index]};
		return reinterpret_cast<T *>(
				reinterpret_cast<std::byte *>(accessor.get()) + misalign);
	}

	void writeImmediate(uintptr_t offset, void *pointer, size_t size) {
		size_t progress = 0;
		while(progress < size) {
			auto misalign = (offset + progress) & (kPageSize - 1);
			auto chunk = frg::min(size - progress, kPageSize - misalign);

			auto index = (offset + progress) >> kPageShift;
			assert(index < _physicalPages.size());
			PageAccessor accessor{_physicalPages[index]};
			memcpy(reinterpret_cast<std::byte *>(accessor.get()) + misalign,
					reinterpret_cast<std::byte *>(pointer) + progress, chunk);
			progress += chunk;
		}
	}

	void readImmediate(uintptr_t offset, void *pointer, size_t size) {
		size_t progress = 0;
		while(progress < size) {
			auto misalign = (offset + progress) & (kPageSize - 1);
			auto chunk = frg::min(size - progress, kPageSize - misalign);

			auto index = (offset + progress) >> kPageShift;
			assert(index < _physicalPages.size());
			PageAccessor accessor{_physicalPages[index]};
			memcpy(reinterpret_cast<std::byte *>(pointer) + progress,
					reinterpret_cast<std::byte *>(accessor.get()) + misalign, chunk);
			progress += chunk;
		}
	}

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<ImmediateMemory> selfPtr;
private:
	frg::ticket_spinlock _mutex;

	frg::vector<PhysicalAddr, KernelAlloc> _physicalPages;
};

struct HardwareMemory final : MemoryView {
	HardwareMemory(PhysicalAddr base, size_t length, CachingMode cache_mode);
	HardwareMemory(const HardwareMemory &) = delete;
	~HardwareMemory();

	HardwareMemory &operator= (const HardwareMemory &) = delete;

	size_t getLength() override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	void markDirty(uintptr_t offset, size_t size) override;

private:
	PhysicalAddr _base;
	size_t _length;
	CachingMode _cacheMode;
};

struct AllocatedMemory final : MemoryView {
	AllocatedMemory(size_t length, int addressBits = 64,
			size_t chunkSize = kPageSize, size_t chunkAlign = kPageSize);
	AllocatedMemory(const AllocatedMemory &) = delete;
	~AllocatedMemory();

	AllocatedMemory &operator= (const AllocatedMemory &) = delete;

	size_t getLength() override;
	coroutine<frg::expected<Error>> resize(size_t newLength) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	void markDirty(uintptr_t offset, size_t size) override;

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<AllocatedMemory> selfPtr;
private:
	frg::ticket_spinlock _mutex;

	frg::vector<PhysicalAddr, KernelAlloc> _physicalChunks;
	int _addressBits;
	size_t _chunkSize, _chunkAlign;
};

struct ManagedSpace : CacheBundle {
	enum LoadState {
		kStateMissing,
		kStatePresent,
		kStateWantInitialization,
		kStateInitialization,
		kStateWantWriteback,
		kStateWriteback,
		kStateAnotherWriteback,
		kStateEvicting
	};

	struct ManagedPage {
		ManagedPage(ManagedSpace *bundle, uint64_t identity) {
			cachePage.bundle = bundle;
			cachePage.identity = identity;
		}

		ManagedPage(const ManagedPage &) = delete;

		ManagedPage &operator= (const ManagedPage &) = delete;

		PhysicalAddr physical = PhysicalAddr(-1);
		LoadState loadState = kStateMissing;
		unsigned int lockCount = 0;
		CachePage cachePage;
	};

	// Calls management callbacks from a WQ; required to implement markDirty().
	struct DeferredManagement {
		void setUp() {
			self->selfPtr.ctr()->increment();
		}

		void execute() {
			ManageList pending;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&self->mutex);

				self->_progressManagement(pending);
			}

			while(!pending.empty()) {
				auto node = pending.pop_front();
				node->complete();
			}

			self->selfPtr.ctr()->decrement();
		}

		ManagedSpace *self;
	};

	ManagedSpace(size_t length, bool readahead);
	~ManagedSpace();

	Error lockPages(uintptr_t offset, size_t size);
	void unlockPages(uintptr_t offset, size_t size);

	void submitManagement(ManageNode *node);
	void submitMonitor(MonitorNode *node);
	void _progressManagement(ManageList &pending);
	void _progressMonitors(MonitorList &pending);

	smarter::borrowed_ptr<ManagedSpace> selfPtr;

	frg::ticket_spinlock mutex;

	frg::rcu_radixtree<ManagedPage, KernelAlloc> pages;

	size_t numPages;
	bool readahead;

	EvictionQueue _evictQueue;

	frg::intrusive_list<
		CachePage,
		frg::locate_member<
			CachePage,
			frg::default_list_hook<CachePage>,
			&CachePage::listHook
		>
	> _initializationList;

	frg::intrusive_list<
		CachePage,
		frg::locate_member<
			CachePage,
			frg::default_list_hook<CachePage>,
			&CachePage::listHook
		>
	> _writebackList;

	ManageList _managementQueue;
	MonitorList _monitorQueue;

	DeferredWork<DeferredManagement> _deferredManagement{{this}};
};

struct BackingMemory final : MemoryView {
public:
	BackingMemory(smarter::shared_ptr<ManagedSpace> managed)
	: MemoryView{&managed->_evictQueue}, _managed{std::move(managed)} { }

	BackingMemory(const BackingMemory &) = delete;

	BackingMemory &operator= (const BackingMemory &) = delete;

	size_t getLength() override;
	coroutine<frg::expected<Error>> resize(size_t newLength) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	void markDirty(uintptr_t offset, size_t size) override;
	void submitManage(ManageNode *handle) override;
	Error updateRange(ManageRequest type, size_t offset, size_t length) override;

private:
	smarter::shared_ptr<ManagedSpace> _managed;
};

struct FrontalMemory final : MemoryView {
public:
	FrontalMemory(smarter::shared_ptr<ManagedSpace> managed)
	: MemoryView{&managed->_evictQueue}, _managed{std::move(managed)} { }

	FrontalMemory(const FrontalMemory &) = delete;

	FrontalMemory &operator= (const FrontalMemory &) = delete;

	size_t getLength() override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	void markDirty(uintptr_t offset, size_t size) override;

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<FrontalMemory> selfPtr;
private:
	smarter::shared_ptr<ManagedSpace> _managed;
};

struct IndirectMemory final : MemoryView {
	IndirectMemory(size_t numSlots);
	IndirectMemory(const IndirectMemory &) = delete;
	~IndirectMemory();

	IndirectMemory &operator= (const IndirectMemory &) = delete;

	size_t getLength() override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	void markDirty(uintptr_t offset, size_t size) override;

	Error setIndirection(size_t slot, smarter::shared_ptr<MemoryView> memory,
			uintptr_t offset, size_t size, CachingFlags flags) override;

private:
	struct IndirectionSlot {
		IndirectionSlot(IndirectMemory *owner, size_t slot,
				smarter::shared_ptr<MemoryView> memory,
				uintptr_t offset, size_t size, CachingFlags flags)
		: owner{owner}, slot{slot}, memory{std::move(memory)}, offset{offset},
			size{size}, flags{flags}, observer{} { }

		IndirectMemory *owner;
		size_t slot;
		smarter::shared_ptr<MemoryView> memory;
		uintptr_t offset;
		size_t size;
		CachingFlags flags;
		MemoryObserver observer;
	};

	frg::ticket_spinlock mutex_;
	frg::vector<smarter::shared_ptr<IndirectionSlot>, KernelAlloc> indirections_;
};

enum class CowState {
	null,
	inProgress,
	hasCopy
};

struct CowPage {
	~CowPage();

	PhysicalAddr physical = -1;
	CowState state = CowState::null;
	unsigned int lockCount = 0;
};

struct CowChain {
	CowChain();

	~CowChain();

// TODO: Either this private again or make this class POD-like.
	frg::ticket_spinlock _mutex;

	frg::rcu_radixtree<smarter::shared_ptr<CowPage>, KernelAlloc> _pages;
};

struct CopyOnWriteMemory final : MemoryView /*, MemoryObserver */ {
public:
	CopyOnWriteMemory(smarter::shared_ptr<MemoryView> view,
			uintptr_t offset, size_t length,
			smarter::shared_ptr<CowChain> chain = nullptr);
	CopyOnWriteMemory(const CopyOnWriteMemory &) = delete;

	~CopyOnWriteMemory();

	CopyOnWriteMemory &operator= (const CopyOnWriteMemory &) = delete;

	size_t getLength() override;
	coroutine<frg::expected<Error, smarter::shared_ptr<MemoryView>>> fork() override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	coroutine<frg::expected<Error, size_t>>
			touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) override;
	void markDirty(uintptr_t offset, size_t size) override;

public:
	// Contract: set by the code that constructs this object.
	smarter::borrowed_ptr<CopyOnWriteMemory> selfPtr;
private:
	frg::ticket_spinlock _mutex;

	smarter::shared_ptr<MemoryView> _view;
	uintptr_t _viewOffset;
	size_t _length;
	smarter::shared_ptr<CowChain> _copyChain;
	frg::rcu_radixtree<smarter::shared_ptr<CowPage>, KernelAlloc> _ownedPages;
	async::recurring_event _copyEvent;
	EvictionQueue _evictQueue;
};

FutexRealm *getGlobalFutexRealm();

} // namespace thor
