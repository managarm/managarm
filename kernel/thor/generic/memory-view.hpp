#pragma once

#include <frg/rcu_radixtree.hpp>
#include <frg/vector.hpp>
#include "error.hpp"
#include "types.hpp"
#include "futex.hpp"
#include "../arch/x86/paging.hpp"

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

	frg::tuple<PhysicalAddr, size_t, CachingMode> range() {
		return _range;
	}

private:
	Worklet *_fetched;
	uint32_t _flags;

	Error _error;
	frg::tuple<PhysicalAddr, size_t, CachingMode> _range;
};

struct EvictNode {
	void setup(Worklet *worklet, size_t pending) {
		_pending.store(pending, std::memory_order_relaxed);
		_worklet = worklet;
	}

	void done() {
		if(_pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
			WorkQueue::post(_worklet);
	}

	bool retirePending(size_t n) {
		return _pending.fetch_sub(n, std::memory_order_acq_rel) == n;
	}

private:
	std::atomic<size_t> _pending;
	Worklet *_worklet;
};

struct MemoryObserver {
	// Called before pages from a MemoryView are evicted.
	// *Important*: While the caller always calls this with a positive RC,
	//              it does *not* keep a reference until EvictNode::complete() is called!
	//              Thus, observeEviction() should increment/decrement the RC itself.
	virtual bool observeEviction(uintptr_t offset, size_t length, EvictNode *node) = 0;

	frg::default_list_hook<MemoryObserver> listHook;
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
		node->_range = frg::tuple<PhysicalAddr, size_t, CachingMode>{physical, size, cm};
	}

	static void callbackFetch(FetchNode *node) {
		WorkQueue::post(node->_fetched);
	}

public:
	virtual size_t getLength() = 0;

	virtual void resize(size_t newLength);

	virtual void copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t length);

	// Add/remove memory observers. These will be notified of page evictions.
	virtual void addObserver(smarter::shared_ptr<MemoryObserver> observer) = 0;
	virtual void removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) = 0;

	// Acquire/release a lock on a memory range.
	// While a lock is active, results of peekRange() and fetchRange() stay consistent.
	// Locks do *not* force all pages to be available, but once a page is available
	// (e.g. due to fetchRange()), it cannot be evicted until the lock is released.
	virtual Error lockRange(uintptr_t offset, size_t size) = 0;
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

struct HardwareMemory final : MemoryView {
	HardwareMemory(PhysicalAddr base, size_t length, CachingMode cache_mode);
	~HardwareMemory();

	size_t getLength() override;
	void addObserver(smarter::shared_ptr<MemoryObserver> observer) override;
	void removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) override;
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
	~AllocatedMemory();

	void copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t length) override;

	size_t getLength() override;
	void resize(size_t newLength) override;
	void addObserver(smarter::shared_ptr<MemoryObserver> observer) override;
	void removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) override;
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

	frg::intrusive_list<
		MemoryObserver,
		frg::locate_member<
			MemoryObserver,
			frg::default_list_hook<MemoryObserver>,
			&MemoryObserver::listHook
		>
	> observers;

	size_t numObservers = 0;

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
	: _managed{std::move(managed)} { }

	size_t getLength() override;
	void resize(size_t newLength) override;
	void addObserver(smarter::shared_ptr<MemoryObserver> observer) override;
	void removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) override;
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
	: _managed{std::move(managed)} { }

	size_t getLength() override;
	void addObserver(smarter::shared_ptr<MemoryObserver> observer) override;
	void removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frg::tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;
	void markDirty(uintptr_t offset, size_t size) override;
	void submitInitiateLoad(MonitorNode *initiate) override;

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

} // namespace thor
