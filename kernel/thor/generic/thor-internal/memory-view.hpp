#pragma once

#include <async/algorithm.hpp>
#include <async/post-ack.hpp>
#include <async/recurring-event.hpp>
#include <frg/expected.hpp>
#include <frg/rcu_radixtree.hpp>
#include <frg/vector.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/paging.hpp>
#include <thor-internal/error.hpp>
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

struct CachePage;

struct ReclaimNode {
	void setup(Worklet *worklet) {
		_worklet = worklet;
	}

	void complete() {
		WorkQueue::post(_worklet);
	}

private:
	Worklet *_worklet;
};

// This is the "backend" part of a memory object.
struct CacheBundle {
	virtual ~CacheBundle() = default;

	virtual bool uncachePage(CachePage *page, ReclaimNode *node) = 0;

	// Called once the reference count of a CachePage reaches zero.
	virtual void retirePage(CachePage *page) = 0;
};

struct CachePage {
	static constexpr uint32_t reclaimStateMask = 0x03;
	// Page is clean and evicatable (part of LRU list).
	static constexpr uint32_t reclaimCached    = 0x01;
	// Page is currently being evicted (not in LRU list).
	static constexpr uint32_t reclaimUncaching  = 0x02;

	// CacheBundle that owns this page.
	CacheBundle *bundle = nullptr;

	// Identity of the page as part of the bundle.
	// Bundles can use this field however they like.
	uint64_t identity = 0;

	// Hooks for LRU lists.
	frg::default_list_hook<CachePage> listHook;

	// To coordinate memory reclaim and the CacheBundle that owns this page,
	// we need a reference counter. This is not related to memory locking.
	std::atomic<uint32_t> refcount = 0;

	uint32_t flags = 0;
};

using PhysicalRange = frg::tuple<PhysicalAddr, size_t, CachingMode>;

struct ManageNode {
	void setup(Worklet *worklet) {
		_worklet = worklet;
	}

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

	void complete() {
		WorkQueue::post(_worklet);
	}

	frg::default_list_hook<ManageNode> processQueueItem;

private:
	// Results of the operation.
	Error _error;
	ManageRequest _type;
	uintptr_t _offset;
	size_t _size;

	Worklet *_worklet;
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
	void setup(ManageRequest type_, uintptr_t offset_, size_t length_, Worklet *worklet) {
		type = type_;
		offset = offset_;
		length = length_;
		_worklet = worklet;
	}

	Error error() { return _error; }

	void setup(Error error) {
		_error = error;
	}

	void complete() {
		WorkQueue::post(_worklet);
	}

	ManageRequest type;
	uintptr_t offset;
	size_t length;

private:
	Error _error;

	Worklet *_worklet;
public:
	frg::default_list_hook<MonitorNode> processQueueItem;

	// Current progress in bytes.
	size_t progress;
};

using InitiateList = frg::intrusive_list<
	MonitorNode,
	frg::locate_member<
		MonitorNode,
		frg::default_list_hook<MonitorNode>,
		&MonitorNode::processQueueItem
	>
>;

using FetchFlags = uint32_t;

struct FetchNode {
	friend struct MemoryView;

	static constexpr FetchFlags disallowBacking = 1;

	void setup(Worklet *fetched, FetchFlags flags = 0) {
		_fetched = fetched;
		_flags = flags;
	}

	FetchFlags flags() {
		return _flags;
	}

	Error error() {
		return _error;
	}

	PhysicalRange range() {
		return _range;
	}

private:
	Worklet *_fetched;
	uint32_t _flags;

	Error _error;
	PhysicalRange _range;
};

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
		auto irqLock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&mutex_);

		observer->agent_.attach(&mechanism_);
		observers_.push_back(observer);
		numObservers_++;
	}

	void removeObserver(MemoryObserver *observer) {
		auto irqLock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&mutex_);

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
	frigg::TicketLock mutex_;

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

struct AddressIdentity {
	friend bool operator== (AddressIdentity lhs, AddressIdentity rhs) {
		return lhs.object == rhs.object
				&& lhs.offset == rhs.offset;
	}

	void *object;
	uintptr_t offset;
};

struct AddressIdentityHash {
	unsigned int operator() (AddressIdentity identity) const {
		return reinterpret_cast<uintptr_t>(identity.object) * 13
				+ identity.offset;
	}
};

// View on some pages of memory. This is the "frontend" part of a memory object.
struct MemoryView {
protected:
	static void completeFetch(FetchNode *node, Error error) {
		node->_error = error;
	}
	static void completeFetch(FetchNode *node, Error error,
			PhysicalAddr physical, size_t size, CachingMode cm) {
		node->_error = error;
		node->_range = PhysicalRange{physical, size, cm};
	}

	static void callbackFetch(FetchNode *node) {
		WorkQueue::post(node->_fetched);
	}

	MemoryView(EvictionQueue *associatedEvictionQueue = nullptr)
	: associatedEvictionQueue_{associatedEvictionQueue} { }

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

	virtual void resize(size_t newLength, async::any_receiver<void> receiver);

	// Returns a unique identity for each memory address.
	// This is used as a key to access futexes.
	virtual frg::expected<Error, AddressIdentity> getAddressIdentity(uintptr_t offset) = 0;

	virtual void fork(async::any_receiver<frg::tuple<Error, frigg::SharedPtr<MemoryView>>> receiver);

	// Acquire/release a lock on a memory range.
	// While a lock is active, results of peekRange() and fetchRange() stay consistent.
	// Locks do *not* force all pages to be available, but once a page is available
	// (e.g. due to fetchRange()), it cannot be evicted until the lock is released.
	virtual Error lockRange(uintptr_t offset, size_t size) = 0;
	virtual void asyncLockRange(uintptr_t offset, size_t size,
			async::any_receiver<Error> receiver);
	virtual void unlockRange(uintptr_t offset, size_t size) = 0;

	// Optimistically returns the physical memory that backs a range of memory.
	// Result stays valid until the range is evicted.
	virtual frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) = 0;

	// Returns the physical memory that backs a range of memory.
	// Ensures that the range is present before returning.
	// Result stays valid until the range is evicted.
	virtual bool fetchRange(uintptr_t offset, FetchNode *node) = 0;

	// Marks a range of pages as dirty.
	virtual void markDirty(uintptr_t offset, size_t size) = 0;

	virtual void submitManage(ManageNode *handle);

	// TODO: InitiateLoad does more or less the same as fetchRange(). Remove it.
	virtual void submitInitiateLoad(MonitorNode *initiate);

	// Called (e.g. by user space) to update a range after loading or writeback.
	virtual Error updateRange(ManageRequest type, size_t offset, size_t length);

	virtual Error setIndirection(size_t slot, frigg::SharedPtr<MemoryView> view,
			uintptr_t offset, size_t size);

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
	// Sender boilerplate for resize()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct ResizeOperation;

	struct [[nodiscard]] ResizeSender {
		template<typename R>
		friend ResizeOperation<R>
		connect(ResizeSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		MemoryView *self;
		size_t newSize;
	};

	ResizeSender resize(size_t newSize) {
		return {this, newSize};
	}

	template<typename R>
	struct ResizeOperation {
		ResizeOperation(ResizeSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		ResizeOperation(const ResizeOperation &) = delete;

		ResizeOperation &operator= (const ResizeOperation &) = delete;

		void start() {
			s_.self->resize(s_.newSize, std::move(receiver_));
		}

	private:
		ResizeSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<ResizeSender>
	operator co_await(ResizeSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for asyncLockRange()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct LockRangeOperation;

	struct [[nodiscard]] LockRangeSender {
		using value_type = Error;

		template<typename R>
		friend LockRangeOperation<R>
		connect(LockRangeSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		MemoryView *self;
		uintptr_t offset;
		size_t size;
	};

	LockRangeSender asyncLockRange(uintptr_t offset, size_t size) {
		return {this, offset, size};
	}

	template<typename R>
	struct LockRangeOperation {
		LockRangeOperation(LockRangeSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		LockRangeOperation(const LockRangeOperation &) = delete;

		LockRangeOperation &operator= (const LockRangeOperation &) = delete;

		void start() {
			s_.self->asyncLockRange(s_.offset, s_.size, std::move(receiver_));
		}

	private:
		LockRangeSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<LockRangeSender, Error>
	operator co_await(LockRangeSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for fetchRange()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct FetchRangeOperation;

	struct [[nodiscard]] FetchRangeSender {
		template<typename R>
		friend FetchRangeOperation<R>
		connect(FetchRangeSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		MemoryView *self;
		uintptr_t offset;
	};

	FetchRangeSender fetchRange(uintptr_t offset) {
		return {this, offset};
	}

	template<typename R>
	struct FetchRangeOperation {
		FetchRangeOperation(FetchRangeSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		FetchRangeOperation(const FetchRangeOperation &) = delete;

		FetchRangeOperation &operator= (const FetchRangeOperation &) = delete;

		bool start_inline() {
			worklet_.setup([] (Worklet *base) {
				auto op = frg::container_of(base, &FetchRangeOperation::worklet_);
				async::execution::set_value_noinline(op->receiver_,
						frg::tuple<Error, PhysicalRange, uint32_t>{op->node_.error(),
								op->node_.range(), op->node_.flags()});
			});
			node_.setup(&worklet_);
			if(s_.self->fetchRange(s_.offset, &node_)) {
				async::execution::set_value_inline(receiver_,
						frg::tuple<Error, PhysicalRange, uint32_t>{node_.error(),
								node_.range(), node_.flags()});
				return true;
			}
			return false;
		}

	private:
		FetchRangeSender s_;
		R receiver_;
		FetchNode node_;
		Worklet worklet_;
	};

	friend async::sender_awaiter<FetchRangeSender, frg::tuple<Error, PhysicalRange, uint32_t>>
	operator co_await(FetchRangeSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------

private:
	EvictionQueue *associatedEvictionQueue_;
};

struct SliceRange {
	MemoryView *view;
	uintptr_t displacement;
	size_t size;
};

struct MemorySlice {
	MemorySlice(frigg::SharedPtr<MemoryView> view,
			ptrdiff_t view_offset, size_t view_size);

	frigg::SharedPtr<MemoryView> getView() {
		return _view;
	}

	uintptr_t offset() { return _viewOffset; }
	size_t length() { return _viewSize; }

private:
	frigg::SharedPtr<MemoryView> _view;
	ptrdiff_t _viewOffset;
	size_t _viewSize;
};

struct TransferNode {
	void setup(MemoryView *dest_memory, uintptr_t dest_offset,
			MemoryView *src_memory, uintptr_t src_offset, size_t length,
			Worklet *copied) {
		_destBundle = dest_memory;
		_srcBundle = src_memory;
		_destOffset = dest_offset;
		_srcOffset = src_offset;
		_size = length;
		_copied = copied;
	}

	MemoryView *_destBundle;
	MemoryView *_srcBundle;
	uintptr_t _destOffset;
	uintptr_t _srcOffset;
	size_t _size;
	Worklet *_copied;

	size_t _progress;
	FetchNode _destFetch;
	FetchNode _srcFetch;
	Worklet _worklet;
};

struct CopyToBundleNode {
	friend bool copyToBundle(MemoryView *, ptrdiff_t, const void *, size_t,
		CopyToBundleNode *, void (*)(CopyToBundleNode *));

private:
	MemoryView *_view;
	uintptr_t _viewOffset;
	const void *_buffer;
	size_t _size;
	void (*_complete)(CopyToBundleNode *);

	size_t _progress;
	Worklet _worklet;
	FetchNode _fetch;
};

struct CopyFromBundleNode {
	friend bool copyFromBundle(MemoryView *, ptrdiff_t, void *, size_t,
		CopyFromBundleNode *, void (*)(CopyFromBundleNode *));

private:
	MemoryView *_view;
	uintptr_t _viewOffset;
	void *_buffer;
	size_t _size;
	void (*_complete)(CopyFromBundleNode *);

	size_t _progress;
	Worklet _worklet;
	FetchNode _fetch;
};

bool transferBetweenViews(TransferNode *node);

bool copyToBundle(MemoryView *view, ptrdiff_t offset, const void *pointer, size_t size,
		CopyToBundleNode *node, void (*complete)(CopyToBundleNode *));

bool copyFromBundle(MemoryView *view, ptrdiff_t offset, void *pointer, size_t size,
		CopyFromBundleNode *node, void (*complete)(CopyFromBundleNode *));

// ----------------------------------------------------------------------------------
// Sender boilerplate for copyToView()
// ----------------------------------------------------------------------------------

template<typename R>
struct CopyToViewOperation;

struct [[nodiscard]] CopyToViewSender {
	using value_type = void;

	template<typename R>
	friend CopyToViewOperation<R>
	connect(CopyToViewSender sender, R receiver) {
		return {sender, std::move(receiver)};
	}

	MemoryView *view;
	uintptr_t offset;
	const void *pointer;
	size_t size;
};

inline CopyToViewSender copyToView(MemoryView *view, uintptr_t offset,
		const void *pointer, size_t size) {
	return {view, offset, pointer, size};
}

template<typename R>
struct CopyToViewOperation {
	CopyToViewOperation(CopyToViewSender s, R receiver)
	: s_{s}, receiver_{std::move(receiver)} { }

	CopyToViewOperation(const CopyToViewOperation &) = delete;

	CopyToViewOperation &operator= (const CopyToViewOperation &) = delete;

	bool start_inline() {
		auto complete = [] (CopyToBundleNode *base) {
			auto op = frg::container_of(base, &CopyToViewOperation::node_);
			async::execution::set_value_noinline(op->receiver_);
		};
		if(copyToBundle(s_.view, s_.offset, s_.pointer, s_.size, &node_, complete)) {
			async::execution::set_value_inline(receiver_);
			return true;
		}
		return false;
	}

private:
	CopyToViewSender s_;
	R receiver_;
	CopyToBundleNode node_;
};

inline async::sender_awaiter<CopyToViewSender, void>
operator co_await(CopyToViewSender sender) {
	return {sender};
}

// ----------------------------------------------------------------------------------
// Sender boilerplate for copyFromView()
// ----------------------------------------------------------------------------------

template<typename R>
struct CopyFromViewOperation;

struct [[nodiscard]] CopyFromViewSender {
	using value_type = void;

	template<typename R>
	friend CopyFromViewOperation<R>
	connect(CopyFromViewSender sender, R receiver) {
		return {sender, std::move(receiver)};
	}

	MemoryView *view;
	uintptr_t offset;
	void *pointer;
	size_t size;
};

inline CopyFromViewSender copyFromView(MemoryView *view, uintptr_t offset,
		void *pointer, size_t size) {
	return {view, offset, pointer, size};
}

template<typename R>
struct CopyFromViewOperation {
	CopyFromViewOperation(CopyFromViewSender s, R receiver)
	: s_{s}, receiver_{std::move(receiver)} { }

	CopyFromViewOperation(const CopyFromViewOperation &) = delete;

	CopyFromViewOperation &operator= (const CopyFromViewOperation &) = delete;

	bool start_inline() {
		auto complete = [] (CopyFromBundleNode *base) {
			auto op = frg::container_of(base, &CopyFromViewOperation::node_);
			async::execution::set_value_noinline(op->receiver_);
		};
		if(copyFromBundle(s_.view, s_.offset, s_.pointer, s_.size, &node_, complete)) {
			async::execution::set_value_inline(receiver_);
			return true;
		}
		return false;
	}

private:
	CopyFromViewSender s_;
	R receiver_;
	CopyFromBundleNode node_;
};

inline async::sender_awaiter<CopyFromViewSender, void>
operator co_await(CopyFromViewSender sender) {
	return {sender};
}

// ----------------------------------------------------------------------------------

struct HardwareMemory final : MemoryView {
	HardwareMemory(PhysicalAddr base, size_t length, CachingMode cache_mode);
	HardwareMemory(const HardwareMemory &) = delete;
	~HardwareMemory();

	HardwareMemory &operator= (const HardwareMemory &) = delete;

	size_t getLength() override;
	frg::expected<Error, AddressIdentity> getAddressIdentity(uintptr_t offset) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;
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
	void resize(size_t newLength, async::any_receiver<void> receiver) override;
	frg::expected<Error, AddressIdentity> getAddressIdentity(uintptr_t offset) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;
	void markDirty(uintptr_t offset, size_t size) override;

private:
	frigg::TicketLock _mutex;

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

	ManagedSpace(size_t length);
	~ManagedSpace();

	bool uncachePage(CachePage *page, ReclaimNode *node) override;

	void retirePage(CachePage *page) override;

	Error lockPages(uintptr_t offset, size_t size);
	void unlockPages(uintptr_t offset, size_t size);

	void submitManagement(ManageNode *node);
	void submitMonitor(MonitorNode *node);
	void _progressManagement();
	void _progressMonitors();

	frigg::TicketLock mutex;

	frg::rcu_radixtree<ManagedPage, KernelAlloc> pages;

	size_t numPages;

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
	InitiateList _monitorQueue;
};

struct BackingMemory final : MemoryView {
public:
	BackingMemory(frigg::SharedPtr<ManagedSpace> managed)
	: MemoryView{&managed->_evictQueue}, _managed{std::move(managed)} { }

	BackingMemory(const BackingMemory &) = delete;

	BackingMemory &operator= (const BackingMemory &) = delete;

	size_t getLength() override;
	void resize(size_t newLength, async::any_receiver<void> receiver) override;
	frg::expected<Error, AddressIdentity> getAddressIdentity(uintptr_t offset) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;
	void markDirty(uintptr_t offset, size_t size) override;
	void submitManage(ManageNode *handle) override;
	Error updateRange(ManageRequest type, size_t offset, size_t length) override;

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

struct FrontalMemory final : MemoryView {
public:
	FrontalMemory(frigg::SharedPtr<ManagedSpace> managed)
	: MemoryView{&managed->_evictQueue}, _managed{std::move(managed)} { }

	FrontalMemory(const FrontalMemory &) = delete;

	FrontalMemory &operator= (const FrontalMemory &) = delete;

	size_t getLength() override;
	frg::expected<Error, AddressIdentity> getAddressIdentity(uintptr_t offset) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;
	void markDirty(uintptr_t offset, size_t size) override;
	void submitInitiateLoad(MonitorNode *initiate) override;

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

struct IndirectMemory final : MemoryView {
	IndirectMemory(size_t numSlots);
	IndirectMemory(const IndirectMemory &) = delete;
	~IndirectMemory();

	IndirectMemory &operator= (const IndirectMemory &) = delete;

	size_t getLength() override;
	frg::expected<Error, AddressIdentity> getAddressIdentity(uintptr_t offset) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;
	void markDirty(uintptr_t offset, size_t size) override;

	Error setIndirection(size_t slot, frigg::SharedPtr<MemoryView> memory,
			uintptr_t offset, size_t size) override;

private:
	struct IndirectionSlot {
		IndirectMemory *owner;
		size_t slot;
		frigg::SharedPtr<MemoryView> memory;
		uintptr_t offset;
		size_t size;
		MemoryObserver observer;
	};

	frigg::TicketLock mutex_;
	frg::vector<smarter::shared_ptr<IndirectionSlot>, KernelAlloc> indirections_;
};

struct CowChain {
	CowChain(frigg::SharedPtr<CowChain> chain);

	~CowChain();

// TODO: Either this private again or make this class POD-like.
	frigg::TicketLock _mutex;

	frigg::SharedPtr<CowChain> _superChain;
	frg::rcu_radixtree<std::atomic<PhysicalAddr>, KernelAlloc> _pages;
};

struct CopyOnWriteMemory final : MemoryView /*, MemoryObserver */ {
public:
	CopyOnWriteMemory(frigg::SharedPtr<MemoryView> view,
			uintptr_t offset, size_t length,
			frigg::SharedPtr<CowChain> chain = nullptr);
	CopyOnWriteMemory(const CopyOnWriteMemory &) = delete;

	~CopyOnWriteMemory();

	CopyOnWriteMemory &operator= (const CopyOnWriteMemory &) = delete;

	size_t getLength() override;
	void fork(async::any_receiver<frg::tuple<Error, frigg::SharedPtr<MemoryView>>> receiver) override;
	frg::expected<Error, AddressIdentity> getAddressIdentity(uintptr_t offset) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void asyncLockRange(uintptr_t offset, size_t size,
			async::any_receiver<Error> receiver) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;
	void markDirty(uintptr_t offset, size_t size) override;

private:
	enum class CowState {
		null,
		inProgress,
		hasCopy
	};

	struct CowPage {
		PhysicalAddr physical = -1;
		CowState state = CowState::null;
		unsigned int lockCount = 0;
	};

	frigg::TicketLock _mutex;

	frigg::SharedPtr<MemoryView> _view;
	uintptr_t _viewOffset;
	size_t _length;
	frigg::SharedPtr<CowChain> _copyChain;
	frg::rcu_radixtree<CowPage, KernelAlloc> _ownedPages;
	async::recurring_event _copyEvent;
	EvictionQueue _evictQueue;
};

} // namespace thor
