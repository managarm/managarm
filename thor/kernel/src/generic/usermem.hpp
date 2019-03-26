#ifndef THOR_GENERIC_USERMEM_HPP
#define THOR_GENERIC_USERMEM_HPP

#include <frigg/vector.hpp>
#include <frg/container_of.hpp>
#include <frg/rbtree.hpp>
#include <frg/rcu_radixtree.hpp>
#include "error.hpp"
#include "types.hpp"
#include "futex.hpp"
#include "../arch/x86/paging.hpp"

namespace thor {

struct Memory;
struct Mapping;
struct AddressSpace;
struct ForeignSpaceAccessor;
struct FaultNode;

using GrabIntent = uint32_t;
enum : GrabIntent {
	kGrabQuery = GrabIntent(1) << 0,
	kGrabFetch = GrabIntent(1) << 1,
	kGrabRead = GrabIntent(1) << 2,
	kGrabWrite = GrabIntent(1) << 3,
	kGrabDontRequireBacking = GrabIntent(1) << 4
};

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

	virtual bool evictPage(CachePage *page, ReclaimNode *node) = 0;

	// Called once the reference count of a CachePage reaches zero.
	virtual void retirePage(CachePage *page) = 0;
};

struct CachePage {
	static constexpr uint32_t reclaimStateMask = 0x03;
	// Page is clean and evicatable (part of LRU list).
	static constexpr uint32_t reclaimCached    = 0x01;
	// Page is currently being evicted (not in LRU list).
	static constexpr uint32_t reclaimEvicting  = 0x02;

	// CacheBundle that owns this page.
	CacheBundle *bundle = nullptr;

	// Hooks for LRU lists.
	frg::default_list_hook<CachePage> lruHook;

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

struct ManageBase {
	void setup(Worklet *worklet) {
		_worklet = worklet;
	}

	Error error() { return _error; }
	uintptr_t offset() { return _offset; }
	size_t size() { return _size; }

	void setup(Error error, uintptr_t offset, size_t size) {
		_error = error;
		_offset = offset;
		_size = size;
	}

	void complete() {
		WorkQueue::post(_worklet);
	}

	frg::default_list_hook<ManageBase> processQueueItem;

private:
	// Results of the operation.
	Error _error;
	uintptr_t _offset;
	size_t _size;

	Worklet *_worklet;
};

using ManageList = frg::intrusive_list<
	ManageBase,
	frg::locate_member<
		ManageBase,
		frg::default_list_hook<ManageBase>,
		&ManageBase::processQueueItem
	>
>;

struct InitiateBase {
	void setup(uintptr_t offset_, size_t length_, Worklet *worklet) {
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

	uintptr_t offset;
	size_t length;

private:
	Error _error;

	Worklet *_worklet;
public:
	frg::default_list_hook<InitiateBase> processQueueItem;

	// Current progress in bytes.
	size_t progress;
};

using InitiateList = frg::intrusive_list<
	InitiateBase,
	frg::locate_member<
		InitiateBase,
		frg::default_list_hook<InitiateBase>,
		&InitiateBase::processQueueItem
	>
>;

struct FetchNode {
	friend struct MemoryView;

	void setup(Worklet *fetched) {
		_fetched = fetched;
	}

	frigg::Tuple<PhysicalAddr, size_t, CachingMode> range() {
		return _range;
	}

private:
	Worklet *_fetched;

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

private:
	std::atomic<size_t> _pending;
	Worklet *_worklet;
};

struct MemoryObserver {
	virtual void evictRange(uintptr_t offset, size_t length, EvictNode *node) = 0;

	frg::default_list_hook<MemoryObserver> listHook;
};

// View on some pages of memory. This is the "frontend" part of a memory object.
struct MemoryView {
protected:
	static void completeFetch(FetchNode *node, PhysicalAddr physical, size_t size,
			CachingMode cm) {
		node->_range = frigg::Tuple<PhysicalAddr, size_t, CachingMode>{physical, size, cm};
	}

	static void callbackFetch(FetchNode *node) {
		WorkQueue::post(node->_fetched);
	}

public:
	// Add/remove memory observers. These will be notified of page evictions.
	virtual void addObserver(MemoryObserver *observer) = 0;
	virtual void removeObserver(MemoryObserver *observer) = 0;

	// Optimistically returns the physical memory that backs a range of memory.
	// Result stays valid until the range is evicted.
	virtual frigg::Tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) = 0;

	// Returns the physical memory that backs a range of memory.
	// Ensures that the range is present before returning.
	// Result stays valid until the range is evicted.
	virtual bool fetchRange(uintptr_t offset, FetchNode *node) = 0;
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

	size_t length();

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
	void submitInitiateLoad(InitiateBase *initiate);

	void submitManage(ManageBase *handle);
	void completeLoad(size_t offset, size_t length);

private:
	MemoryTag _tag;
};

struct CopyToBundleNode {
	friend void copyToBundle(Memory *, ptrdiff_t, const void *, size_t,
		CopyToBundleNode *, void (*)(CopyToBundleNode *));

private:
	Worklet _worklet;
	FetchNode _fetch;
};

struct CopyFromBundleNode {
	friend void copyFromBundle(Memory *, ptrdiff_t, void *, size_t,
		CopyFromBundleNode *, void (*)(CopyFromBundleNode *));

private:
	Worklet _worklet;
	FetchNode _fetch;
};

void copyToBundle(Memory *view, ptrdiff_t offset, const void *pointer, size_t size,
		CopyToBundleNode *node, void (*complete)(CopyToBundleNode *));

void copyFromBundle(Memory *view, ptrdiff_t offset, void *pointer, size_t size,
		CopyFromBundleNode *node, void (*complete)(CopyFromBundleNode *));

struct HardwareMemory : Memory {
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::hardware;
	}

	HardwareMemory(PhysicalAddr base, size_t length, CachingMode cache_mode);
	~HardwareMemory();

	void addObserver(MemoryObserver *observer) override;
	void removeObserver(MemoryObserver *observer) override;
	frigg::Tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;

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

	void addObserver(MemoryObserver *observer) override;
	void removeObserver(MemoryObserver *observer) override;
	frigg::Tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;

	size_t getLength();

private:
	frigg::TicketLock _mutex;

	frigg::Vector<PhysicalAddr, KernelAlloc> _physicalChunks;
	size_t _chunkSize, _chunkAlign;
};

struct ManagedSpace : CacheBundle {
	enum LoadState {
		kStateMissing,
		kStateLoading,
		kStateLoaded
	};

	ManagedSpace(size_t length);
	~ManagedSpace();

	bool evictPage(CachePage *page, ReclaimNode *node) override;

	void retirePage(CachePage *page) override;

	void progressLoads();
	bool isComplete(InitiateBase *initiate);

	frigg::TicketLock mutex;

	// TODO: Store all of this information in a radix tree.
	frigg::Vector<PhysicalAddr, KernelAlloc> physicalPages;
	frigg::Vector<LoadState, KernelAlloc> loadState;
	// TODO: Use a unique_ptr to manage this array.
	CachePage *pages;

	frg::intrusive_list<
		MemoryObserver,
		frg::locate_member<
			MemoryObserver,
			frg::default_list_hook<MemoryObserver>,
			&MemoryObserver::listHook
		>
	> observers;

	size_t numObservers = 0;

	InitiateList initiateLoadQueue;
	InitiateList pendingLoadQueue;
	InitiateList completedLoadQueue;

	ManageList submittedManageQueue;
	ManageList completedManageQueue;
};

struct BackingMemory : Memory {
public:
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::backing;
	}

	BackingMemory(frigg::SharedPtr<ManagedSpace> managed)
	: Memory(MemoryTag::backing), _managed(frigg::move(managed)) { }

	void addObserver(MemoryObserver *observer) override;
	void removeObserver(MemoryObserver *observer) override;
	frigg::Tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;

	size_t getLength();

	void submitManage(ManageBase *handle);
	void completeLoad(size_t offset, size_t length);

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

	void addObserver(MemoryObserver *observer) override;
	void removeObserver(MemoryObserver *observer) override;
	frigg::Tuple<PhysicalAddr, CachingMode> peekRange(uintptr_t offset) override;
	bool fetchRange(uintptr_t offset, FetchNode *node) override;

	size_t getLength();

	void submitInitiateLoad(InitiateBase *initiate);

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

struct PrepareNode {
	void setup(uintptr_t offset, size_t size, Worklet *prepared) {
		_offset = offset;
		_size = size;
		_prepared = prepared;
	}

	uintptr_t _offset;
	size_t _size;
	Worklet *_prepared;
};

struct Mapping {
	Mapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags);

	virtual ~Mapping() { }

	AddressSpace *owner() {
		return _owner;
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

protected:

public:
	virtual frigg::Tuple<PhysicalAddr, CachingMode>
	resolveRange(ptrdiff_t offset) = 0;

	virtual bool prepareRange(PrepareNode *node) = 0;

	virtual Mapping *shareMapping(AddressSpace *dest_space) = 0;
	virtual Mapping *copyOnWrite(AddressSpace *dest_space) = 0;

	virtual void install(bool overwrite) = 0;
	virtual void uninstall(bool clear) = 0;

	frg::rbtree_hook treeNode;

private:
	AddressSpace *_owner;
	VirtualAddr _address;
	size_t _length;
	MappingFlags _flags;
};

struct NormalMapping : Mapping, MemoryObserver {
	NormalMapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags, frigg::SharedPtr<MemorySlice> view, uintptr_t offset);

	frigg::Tuple<PhysicalAddr, CachingMode>
	resolveRange(ptrdiff_t offset) override;

	bool prepareRange(PrepareNode *node) override;

	Mapping *shareMapping(AddressSpace *dest_space) override;
	Mapping *copyOnWrite(AddressSpace *dest_space) override;

	void install(bool overwrite) override;
	void uninstall(bool clear) override;

	void evictRange(uintptr_t offset, size_t length, EvictNode *node) override;

private:
	frigg::SharedPtr<MemorySlice> _slice;
	frigg::SharedPtr<MemoryView> _view;
	size_t _offset;
};

struct CowChain {
	CowChain(frigg::SharedPtr<MemorySlice> view, ptrdiff_t offset, size_t size);

	CowChain(frigg::SharedPtr<CowChain> chain, ptrdiff_t offset, size_t size);

	~CowChain();

// TODO: Either this private again or make this class POD-like.
	frigg::TicketLock _mutex;

	frigg::SharedPtr<MemorySlice> _superRoot;
	frigg::SharedPtr<CowChain> _superChain;
	ptrdiff_t _superOffset;
	frg::rcu_radixtree<std::atomic<PhysicalAddr>, KernelAlloc> _pages;
	frigg::SharedPtr<Memory> _copy;
};

struct CowMapping : Mapping {
	CowMapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags, frigg::SharedPtr<CowChain> chain);

	frigg::Tuple<PhysicalAddr, CachingMode>
	resolveRange(ptrdiff_t offset) override;

	bool prepareRange(PrepareNode *node) override;

	Mapping *shareMapping(AddressSpace *dest_space) override;
	Mapping *copyOnWrite(AddressSpace *dest_space) override;

	void install(bool overwrite) override;
	void uninstall(bool clear) override;

private:
	frigg::SharedPtr<CowChain> _chain;
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

	Mapping *_mapping;
	Worklet _worklet;
	PrepareNode _prepare;
};

struct ForkItem {
	Mapping *mapping;
	AllocatedMemory *destBundle;
};

struct ForkNode {
	friend struct AddressSpace;

	ForkNode()
	: _items{*kernelAlloc} { }

	void setup(Worklet *forked) {
		_forked = forked;
	}
	frigg::SharedPtr<AddressSpace> forkedSpace() {
		return frigg::move(_fork);
	}

private:
	Worklet *_forked;

	// TODO: This should be a SharedPtr, too.
	AddressSpace *_original;
	frigg::SharedPtr<AddressSpace> _fork;
	frigg::LinkedList<ForkItem, KernelAlloc> _items;
	Worklet _worklet;
	PrepareNode _prepare;
	size_t _progress;
};

struct AddressUnmapNode {
	friend struct AddressSpace;

private:
	AddressSpace *_space;
	ShootNode _shootNode;
};

class AddressSpace : frigg::SharedCounter {
	friend struct ForeignSpaceAccessor;
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

	static frigg::SharedPtr<AddressSpace> create() {
		auto space = frigg::construct<AddressSpace>(*kernelAlloc);
		return frigg::SharedPtr<AddressSpace>{frigg::adoptShared, space, 
				frigg::SharedControl{space}};
	}

	static void activate(frigg::SharedPtr<AddressSpace> space);

	AddressSpace();

	~AddressSpace();

	void destruct() override; // Called when shared_ptr refcount reaches zero.
	void cleanup() override; // Called when weak_ptr refcount reaches zero.

	void setupDefaultMappings();

	Error map(Guard &guard, frigg::UnsafePtr<MemorySlice> view,
			VirtualAddr address, size_t offset, size_t length,
			uint32_t flags, VirtualAddr *actual_address);

	void unmap(Guard &guard, VirtualAddr address, size_t length,
			AddressUnmapNode *node);

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

	Mapping *_getMapping(VirtualAddr address);

	// Splits some memory range from a hole mapping.
	void _splitHole(Hole *hole, VirtualAddr offset, VirtualAddr length);

	HoleTree _holes;
	MappingTree _mappings;

	ClientPageSpace _pageSpace;

	int64_t _residuentSize = 0;
};

struct AcquireNode {
	friend struct ForeignSpaceAccessor;

	AcquireNode()
	: _acquired{nullptr} { }

	AcquireNode(const AcquireNode &) = delete;

	AcquireNode &operator= (const AcquireNode &) = delete;

	void setup(Worklet *acquire) {
		_acquired = acquire;
	}

private:
	Worklet *_acquired;

	ForeignSpaceAccessor *_accessor;
	Worklet _worklet;
	PrepareNode _prepare;
};

struct ForeignSpaceAccessor {
public:
	friend void swap(ForeignSpaceAccessor &a, ForeignSpaceAccessor &b) {
		frigg::swap(a._space, b._space);
		frigg::swap(a._address, b._address);
		frigg::swap(a._length, b._length);
		frigg::swap(a._acquired, b._acquired);
	}

	ForeignSpaceAccessor()
	: _acquired{false} { }

	ForeignSpaceAccessor(frigg::SharedPtr<AddressSpace> space,
			void *address, size_t length)
	: _space(frigg::move(space)), _address(address), _length(length),
			_acquired{false} { }

	ForeignSpaceAccessor(const ForeignSpaceAccessor &other) = delete;

	ForeignSpaceAccessor(ForeignSpaceAccessor &&other)
	: ForeignSpaceAccessor() {
		swap(*this, other);
	}

	ForeignSpaceAccessor &operator= (ForeignSpaceAccessor other) {
		swap(*this, other);
		return *this;
	}

	frigg::UnsafePtr<AddressSpace> space() {
		return _space;
	}
	uintptr_t address() {
		return (uintptr_t)_address;
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

	frigg::SharedPtr<AddressSpace> _space;
	void *_address;
	size_t _length;
	bool _acquired;
};

void initializeReclaim();

} // namespace thor

#endif // THOR_GENERIC_USERMEM_HPP
