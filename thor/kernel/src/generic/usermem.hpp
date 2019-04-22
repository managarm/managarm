#ifndef THOR_GENERIC_USERMEM_HPP
#define THOR_GENERIC_USERMEM_HPP

#include <frigg/vector.hpp>
#include <frg/container_of.hpp>
#include <frg/rbtree.hpp>
#include <frg/rcu_radixtree.hpp>
#include <frg/vector.hpp>
#include <smarter.hpp>
#include "error.hpp"
#include "mm-rc.hpp"
#include "types.hpp"
#include "futex.hpp"
#include "../arch/x86/paging.hpp"

namespace thor {

enum class ManageRequest {
	null,
	initialize,
	writeback
};

struct Memory;
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

enum class MemoryTag {
	null,
	hardware,
	allocated,
	backing,
	frontal,
	copyOnWrite
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

	frigg::Tuple<PhysicalAddr, size_t, CachingMode> range() {
		return _range;
	}

private:
	Worklet *_fetched;
	uint32_t _flags;

	Error _error;
	frigg::Tuple<PhysicalAddr, size_t, CachingMode> _range;
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
		node->_range = frigg::Tuple<PhysicalAddr, size_t, CachingMode>{physical, size, cm};
	}

	static void callbackFetch(FetchNode *node) {
		WorkQueue::post(node->_fetched);
	}

public:
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
	virtual frigg::Tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) = 0;

	// Returns the physical memory that backs a range of memory.
	// Ensures that the range is present before returning.
	// Result stays valid until the range is evicted.
	virtual bool fetchRange(uintptr_t offset, FetchNode *node) = 0;

	// Marks a range of pages as dirty.
	virtual void markDirty(uintptr_t offset, size_t size) = 0;

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

	SliceRange translateRange(ptrdiff_t offset, size_t size);

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

struct Memory : MemoryView {
	static bool transfer(TransferNode *node);

	Memory(MemoryTag tag)
	: _tag(tag) { }

	Memory(const Memory &) = delete;

	Memory &operator= (const Memory &) = delete;

	MemoryTag tag() const {
		return _tag;
	}

	virtual void resize(size_t new_length);

	virtual void copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t length);

	size_t getLength();

	// TODO: InitiateLoad does more or less the same as fetchRange(). Remove it.
	void submitInitiateLoad(MonitorNode *initiate);

	void submitManage(ManageNode *handle);

private:
	MemoryTag _tag;
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

bool copyToBundle(MemoryView *view, ptrdiff_t offset, const void *pointer, size_t size,
		CopyToBundleNode *node, void (*complete)(CopyToBundleNode *));

bool copyFromBundle(MemoryView *view, ptrdiff_t offset, void *pointer, size_t size,
		CopyFromBundleNode *node, void (*complete)(CopyFromBundleNode *));

struct HardwareMemory : Memory {
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::hardware;
	}

	HardwareMemory(PhysicalAddr base, size_t length, CachingMode cache_mode);
	~HardwareMemory();

	void addObserver(smarter::shared_ptr<MemoryObserver> observer) override;
	void removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frigg::Tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;
	void markDirty(uintptr_t offset, size_t size) override;

	size_t getLength();

private:
	PhysicalAddr _base;
	size_t _length;
	CachingMode _cacheMode;
};

struct AllocatedMemory : Memory {
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::allocated;
	}

	AllocatedMemory(size_t length, size_t chunk_size = kPageSize,
			size_t chunk_align = kPageSize);
	~AllocatedMemory();

	void resize(size_t new_length) override;

	void copyKernelToThisSync(ptrdiff_t offset, void *pointer, size_t length) override;

	void addObserver(smarter::shared_ptr<MemoryObserver> observer) override;
	void removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frigg::Tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;
	void markDirty(uintptr_t offset, size_t size) override;

	size_t getLength();

private:
	frigg::TicketLock _mutex;

	frigg::Vector<PhysicalAddr, KernelAlloc> _physicalChunks;
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

struct BackingMemory : Memory {
public:
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::backing;
	}

	BackingMemory(frigg::SharedPtr<ManagedSpace> managed)
	: Memory(MemoryTag::backing), _managed(frigg::move(managed)) { }

	void resize(size_t new_length) override;

	void addObserver(smarter::shared_ptr<MemoryObserver> observer) override;
	void removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frigg::Tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;
	void markDirty(uintptr_t offset, size_t size) override;

	size_t getLength();

	void submitManage(ManageNode *handle);
	Error updateRange(ManageRequest type, size_t offset, size_t length) override;

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

struct FrontalMemory : Memory {
public:
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::frontal;
	}

	FrontalMemory(frigg::SharedPtr<ManagedSpace> managed)
	: Memory(MemoryTag::frontal), _managed(frigg::move(managed)) { }

	void addObserver(smarter::shared_ptr<MemoryObserver> observer) override;
	void removeObserver(smarter::borrowed_ptr<MemoryObserver> observer) override;
	Error lockRange(uintptr_t offset, size_t size) override;
	void unlockRange(uintptr_t offset, size_t size) override;
	frigg::Tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;
	void markDirty(uintptr_t offset, size_t size) override;

	size_t getLength();

	void submitInitiateLoad(MonitorNode *initiate);

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

struct Hole {
	Hole(VirtualAddr address, size_t length)
	: _address{address}, _length{length}, largestHole{0} { }

	VirtualAddr address() const {
		return _address;
	}

	size_t length() const {
		return _length;
	}

	frg::rbtree_hook treeNode;

private:
	VirtualAddr _address;
	size_t _length;

public:
	// Largest hole in the subtree of this node.
	size_t largestHole;
};

enum MappingFlags : uint32_t {
	null = 0,

	forkMask = 0x07,
	dropAtFork = 0x01,
	shareAtFork = 0x02,
	copyOnWriteAtFork = 0x04,

	permissionMask = 0x70,
	protRead = 0x10,
	protWrite = 0x20,
	protExecute = 0x40,

	dontRequireBacking = 0x100
};

struct LockVirtualNode {
	static void post(LockVirtualNode *node) {
		WorkQueue::post(node->_worklet);
	}

	void setup(uintptr_t offset, size_t size, Worklet *worklet) {
		_offset = offset;
		_size = size;
		_worklet = worklet;
	}

	auto offset() { return _offset; }
	auto size() { return _size; }

private:
	uintptr_t _offset;
	size_t _size;
	Worklet *_worklet;
};

struct TouchVirtualNode {
	void setup(uintptr_t offset, Worklet *worklet) {
		_offset = offset;
		_worklet = worklet;
	}

	void setResult(Error error) {
		_error = error;
	}
	void setResult(Error error, frigg::Tuple<PhysicalAddr, size_t, CachingMode> range,
			bool spurious = false) {
		_error = error;
		_range = range;
		_spurious = spurious;
	}
	void setResult(Error error, PhysicalAddr physical, size_t size, CachingMode mode,
			bool spurious = false) {
		_error = error;
		_range = frigg::Tuple<PhysicalAddr, size_t, CachingMode>{physical, size, mode};
		_spurious = spurious;
	}

	Error error() {
		return _error;
	}

	frigg::Tuple<PhysicalAddr, size_t, CachingMode> range() {
		return _range;
	}

	bool spurious() {
		return _spurious;
	}

	uintptr_t _offset;
	Worklet *_worklet;

private:
	Error _error;
	frigg::Tuple<PhysicalAddr, size_t, CachingMode> _range;
	bool _spurious;
};

struct PopulateVirtualNode {
	void setup(uintptr_t offset, size_t size, Worklet *prepared) {
		_offset = offset;
		_size = size;
		_prepared = prepared;
	}

	uintptr_t _offset;
	size_t _size;
	Worklet *_prepared;
};

enum class MappingState {
	null,
	active,
	zombie,
	retired
};

struct Mapping {
	Mapping(size_t length, MappingFlags flags);

	Mapping(const Mapping &) = delete;

	virtual ~Mapping() = default;

	Mapping &operator= (const Mapping &) = delete;

	AddressSpace *owner() {
		return _owner.get();
	}

	VirtualAddr address() const {
		return _address;
	}

	size_t length() const {
		return _length;
	}

	MappingFlags flags() const {
		return _flags;
	}

public:
	void tie(smarter::shared_ptr<AddressSpace> owner, VirtualAddr address);

	// Makes sure that pages are not evicted from virtual memory.
	virtual bool lockVirtualRange(LockVirtualNode *node) = 0;
	virtual void unlockVirtualRange(uintptr_t offset, size_t length) = 0;

	virtual frigg::Tuple<PhysicalAddr, CachingMode>
	resolveRange(ptrdiff_t offset) = 0;

	// Ensures that a page of virtual memory is present.
	// Note that this does *not* guarantee that the page is not evicted immediately,
	// unless you hold a lock (via lockVirtualRange()).
	virtual bool touchVirtualPage(TouchVirtualNode *node) = 0;

	// Helper function that calls touchVirtualPage() on a certain range.
	bool populateVirtualRange(PopulateVirtualNode *node);

	virtual smarter::shared_ptr<Mapping> forkMapping() = 0;

	virtual void install() = 0;
	virtual void uninstall() = 0;
	virtual void retire() = 0;

	smarter::borrowed_ptr<Mapping> selfPtr;

	frg::rbtree_hook treeNode;

protected:
	uint32_t compilePageFlags();

private:
	smarter::shared_ptr<AddressSpace> _owner;
	VirtualAddr _address;
	size_t _length;
	MappingFlags _flags;
};

struct NormalMapping : Mapping, MemoryObserver {
	friend struct AddressSpace;

	NormalMapping(size_t length, MappingFlags flags,
			frigg::SharedPtr<MemorySlice> view, uintptr_t offset);

	~NormalMapping();

	bool lockVirtualRange(LockVirtualNode *node) override;
	void unlockVirtualRange(uintptr_t offset, size_t length) override;
	frigg::Tuple<PhysicalAddr, CachingMode> resolveRange(ptrdiff_t offset) override;
	bool touchVirtualPage(TouchVirtualNode *node) override;

	smarter::shared_ptr<Mapping> forkMapping() override;

	void install() override;
	void uninstall() override;
	void retire() override;

	bool observeEviction(uintptr_t offset, size_t length, EvictNode *node) override;

private:
	MappingState _state = MappingState::null;
	frigg::SharedPtr<MemorySlice> _slice;
	frigg::SharedPtr<MemoryView> _view;
	size_t _viewOffset;
};

struct CowChain {
	CowChain(frigg::SharedPtr<CowChain> chain);

	~CowChain();

// TODO: Either this private again or make this class POD-like.
	frigg::TicketLock _mutex;

	frigg::SharedPtr<CowChain> _superChain;
	frg::rcu_radixtree<std::atomic<PhysicalAddr>, KernelAlloc> _pages;
};

struct CowMapping : Mapping, MemoryObserver {
	friend struct AddressSpace;

	CowMapping(size_t length, MappingFlags flags,
			frigg::SharedPtr<MemorySlice> slice, uintptr_t view_offset,
			frigg::SharedPtr<CowChain> chain);

	~CowMapping();

	bool lockVirtualRange(LockVirtualNode *node) override;
	void unlockVirtualRange(uintptr_t offset, size_t length) override;
	frigg::Tuple<PhysicalAddr, CachingMode> resolveRange(ptrdiff_t offset) override;
	bool touchVirtualPage(TouchVirtualNode *node) override;

	smarter::shared_ptr<Mapping> forkMapping() override;

	void install() override;
	void uninstall() override;
	void retire() override;

	bool observeEviction(uintptr_t offset, size_t length, EvictNode *node) override;

private:
	struct CowPage {
		PhysicalAddr physical = -1;
		unsigned int lockCount = 0;
	};

	frigg::TicketLock _mutex;

	frigg::SharedPtr<MemorySlice> _slice;
	uintptr_t _viewOffset;
	frigg::SharedPtr<CowChain> _copyChain;

	MappingState _state = MappingState::null;
	frg::rcu_radixtree<CowPage, KernelAlloc> _ownedPages;
};

struct HoleLess {
	bool operator() (const Hole &a, const Hole &b) {
		return a.address() < b.address();
	}
};

struct HoleAggregator;

using HoleTree = frg::rbtree<
	Hole,
	&Hole::treeNode,
	HoleLess,
	HoleAggregator
>;

struct HoleAggregator {
	static bool aggregate(Hole *node);
	static bool check_invariant(HoleTree &tree, Hole *node);
};

struct MappingLess {
	bool operator() (const Mapping &a, const Mapping &b) {
		return a.address() < b.address();
	}
};

using MappingTree = frg::rbtree<
	Mapping,
	&Mapping::treeNode,
	MappingLess
>;

struct FaultNode {
	friend struct AddressSpace;
	friend struct NormalMapping;
	friend struct CowMapping;

	FaultNode()
	: _resolved{false} { }

	FaultNode(const FaultNode &) = delete;

	FaultNode &operator= (const FaultNode &) = delete;

	void setup(Worklet *handled) {
		_handled = handled;
	}

	bool resolved() {
		return _resolved;
	}

private:
	VirtualAddr _address;
	uint32_t _flags;
	Worklet *_handled;

	bool _resolved;

	smarter::shared_ptr<Mapping> _mapping;
	Worklet _worklet;
	TouchVirtualNode _touchVirtual;
};

struct ForkItem {
	Mapping *mapping;
};

struct ForkNode {
	friend struct AddressSpace;

	ForkNode()
	: _items{*kernelAlloc} { }

	void setup(Worklet *forked) {
		_forked = forked;
	}
	smarter::shared_ptr<AddressSpace, BindableHandle> forkedSpace() {
		return frigg::move(_fork);
	}

private:
	Worklet *_forked;

	// TODO: This should be a SharedPtr, too.
	AddressSpace *_original;
	smarter::shared_ptr<AddressSpace, BindableHandle> _fork;
	frigg::LinkedList<ForkItem, KernelAlloc> _items;
	Worklet _worklet;
	ShootNode _shootNode;
};

struct AddressUnmapNode {
	friend struct AddressSpace;

	void setup(Worklet *completion) {
		_completion = completion;
	}

	void complete() {
		WorkQueue::post(_completion);
	}

private:
	Worklet *_completion;
	AddressSpace *_space;
	smarter::shared_ptr<Mapping> _mapping;
	Worklet _worklet;
	ShootNode _shootNode;
};

struct AddressSpace : smarter::crtp_counter<AddressSpace, BindableHandle> {
	friend struct AddressSpaceLockHandle;
	friend struct NormalMapping;
	friend struct CowMapping;

public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	typedef uint32_t MapFlags;
	enum : MapFlags {
		kMapShareAtFork = 0x80,
		kMapCopyOnWrite = 0x800,

		kMapFixed = 0x01,
		kMapPreferBottom = 0x02,
		kMapPreferTop = 0x04,
		kMapProtRead = 0x08,
		kMapProtWrite = 0x10,
		kMapProtExecute = 0x20,
		kMapDropAtFork = 0x40,
		kMapCopyOnWriteAtFork = 0x100,
		kMapPopulate = 0x200,
		kMapDontRequireBacking = 0x400,
	};

	enum FaultFlags : uint32_t {
		kFaultWrite = (1 << 1),
		kFaultExecute = (1 << 2)
	};

	static smarter::shared_ptr<AddressSpace, BindableHandle>
	constructHandle(smarter::shared_ptr<AddressSpace> ptr) {
		auto space = ptr.get();
		space->setup(smarter::adopt_rc, ptr.ctr(), 1);
		ptr.release();
		return smarter::shared_ptr<AddressSpace, BindableHandle>{smarter::adopt_rc, space, space};
	}

	static smarter::shared_ptr<AddressSpace, BindableHandle> create() {
		auto ptr = smarter::allocate_shared<AddressSpace>(Allocator{});
		ptr->selfPtr = ptr;
		return constructHandle(std::move(ptr));
	}

	static void activate(smarter::shared_ptr<AddressSpace, BindableHandle> space);

	AddressSpace();

	~AddressSpace();

	void dispose(BindableHandle);

	void setupDefaultMappings();

	smarter::shared_ptr<Mapping> getMapping(VirtualAddr address);

	Error map(Guard &guard, frigg::UnsafePtr<MemorySlice> view,
			VirtualAddr address, size_t offset, size_t length,
			uint32_t flags, VirtualAddr *actual_address);

	bool unmap(VirtualAddr address, size_t length, AddressUnmapNode *node);

	bool handleFault(VirtualAddr address, uint32_t flags, FaultNode *node);

	bool fork(ForkNode *node);

	size_t rss() {
		return _residuentSize;
	}

	Lock lock;

	Futex futexSpace;

private:

	// Allocates a new mapping of the given length somewhere in the address space.
	VirtualAddr _allocate(size_t length, MapFlags flags);

	VirtualAddr _allocateAt(VirtualAddr address, size_t length);

	smarter::shared_ptr<Mapping> _findMapping(VirtualAddr address);

	// Splits some memory range from a hole mapping.
	void _splitHole(Hole *hole, VirtualAddr offset, VirtualAddr length);

	smarter::borrowed_ptr<AddressSpace> selfPtr;

	HoleTree _holes;
	MappingTree _mappings;

	ClientPageSpace _pageSpace;

	int64_t _residuentSize = 0;
};

struct MemoryViewLockHandle {
	friend void swap(MemoryViewLockHandle &a, MemoryViewLockHandle &b) {
		frigg::swap(a._view, b._view);
		frigg::swap(a._offset, b._offset);
		frigg::swap(a._size, b._size);
		frigg::swap(a._active, b._active);
	}

	MemoryViewLockHandle() = default;

	MemoryViewLockHandle(frigg::SharedPtr<MemoryView> view, uintptr_t offset, size_t size);

	MemoryViewLockHandle(const MemoryViewLockHandle &) = delete;

	MemoryViewLockHandle(MemoryViewLockHandle &&other)
	: MemoryViewLockHandle{} {
		swap(*this, other);
	}

	~MemoryViewLockHandle();

	MemoryViewLockHandle &operator= (MemoryViewLockHandle other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return _active;
	}

private:
	frigg::SharedPtr<MemoryView> _view = nullptr;
	uintptr_t _offset = 0;
	size_t _size = 0;
	bool _active = false;
};

struct AcquireNode {
	friend struct AddressSpaceLockHandle;

	AcquireNode()
	: _acquired{nullptr} { }

	AcquireNode(const AcquireNode &) = delete;

	AcquireNode &operator= (const AcquireNode &) = delete;

	void setup(Worklet *acquire) {
		_acquired = acquire;
	}

private:
	Worklet *_acquired;

	AddressSpaceLockHandle *_accessor;
	Worklet _worklet;
	LockVirtualNode _lockNode;
	PopulateVirtualNode _populateNode;
};

struct AddressSpaceLockHandle {
public:
	friend void swap(AddressSpaceLockHandle &a, AddressSpaceLockHandle &b) {
		frigg::swap(a._space, b._space);
		frigg::swap(a._mapping, b._mapping);
		frigg::swap(a._address, b._address);
		frigg::swap(a._length, b._length);
		frigg::swap(a._active, b._active);
	}

	AddressSpaceLockHandle() = default;

	AddressSpaceLockHandle(smarter::shared_ptr<AddressSpace, BindableHandle> space,
			void *pointer, size_t length);

	AddressSpaceLockHandle(const AddressSpaceLockHandle &other) = delete;

	AddressSpaceLockHandle(AddressSpaceLockHandle &&other)
	: AddressSpaceLockHandle() {
		swap(*this, other);
	}

	~AddressSpaceLockHandle();

	AddressSpaceLockHandle &operator= (AddressSpaceLockHandle other) {
		swap(*this, other);
		return *this;
	}

	smarter::borrowed_ptr<AddressSpace, BindableHandle> space() {
		return _space;
	}
	uintptr_t address() {
		return _address;
	}
	size_t length() {
		return _length;
	}

	bool acquire(AcquireNode *node);

	PhysicalAddr getPhysical(size_t offset);

	void load(size_t offset, void *pointer, size_t size);
	Error write(size_t offset, const void *pointer, size_t size);

	template<typename T>
	T read(size_t offset) {
		T value;
		load(offset, &value, sizeof(T));
		return value;
	}

	template<typename T>
	Error write(size_t offset, T value) {
		return write(offset, &value, sizeof(T));
	}

private:
	PhysicalAddr _resolvePhysical(VirtualAddr vaddr);

	smarter::shared_ptr<AddressSpace, BindableHandle> _space;
	smarter::shared_ptr<Mapping> _mapping;
	uintptr_t _address = 0;
	size_t _length = 0;
	bool _active = false; // Whether the accessor is acquired successfully.
};

struct NamedMemoryViewLock {
	NamedMemoryViewLock(MemoryViewLockHandle handle)
	: _handle{std::move(handle)} { }

	NamedMemoryViewLock(const NamedMemoryViewLock &) = delete;

	~NamedMemoryViewLock();

	NamedMemoryViewLock &operator= (const NamedMemoryViewLock &) = delete;

private:
	MemoryViewLockHandle _handle;
};

void initializeReclaim();

} // namespace thor

#endif // THOR_GENERIC_USERMEM_HPP
