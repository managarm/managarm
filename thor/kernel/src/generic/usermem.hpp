#ifndef THOR_GENERIC_USERMEM_HPP
#define THOR_GENERIC_USERMEM_HPP

#include <frigg/rbtree.hpp>
#include <frigg/vector.hpp>
#include <frg/rcu_radixtree.hpp>
#include "error.hpp"
#include "types.hpp"
#include "futex.hpp"
#include "../arch/x86/paging.hpp"

namespace thor {

struct Memory;
struct AddressSpace;

using GrabIntent = uint32_t;
enum : GrabIntent {
	kGrabQuery = GrabIntent(1) << 0,
	kGrabFetch = GrabIntent(1) << 1,
	kGrabRead = GrabIntent(1) << 2,
	kGrabWrite = GrabIntent(1) << 3,
	kGrabDontRequireBacking = GrabIntent(1) << 4
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
	virtual void complete(Error error, uintptr_t offset, size_t size) = 0;

	frigg::IntrusiveSharedLinkedItem<ManageBase> processQueueItem;
};

template<typename F>
struct Manage : ManageBase {
	Manage(F functor)
	: _functor(frigg::move(functor)) { }

	void complete(Error error, uintptr_t offset, size_t size) override {
		_functor(error, offset, size);
	}

private:
	F _functor;
};

struct InitiateBase {
	InitiateBase(size_t offset, size_t length)
	: offset(offset), length(length), progress(0) { }

	virtual void complete(Error error) = 0;
	
	size_t offset;
	size_t length;

	// Current progress in bytes.
	size_t progress;
	

	frigg::IntrusiveSharedLinkedItem<InitiateBase> processQueueItem;
};

template<typename F>
struct Initiate : InitiateBase {
	Initiate(size_t offset, size_t length, F functor)
	: InitiateBase(offset, length), _functor(frigg::move(functor)) { }

	void complete(Error error) override {
		_functor(error);
	}

private:
	F _functor;
};

struct MemoryBundle {	
	// Optimistically returns the physical memory that backs a range of memory.
	// Result stays valid until the range is evicted.
	virtual PhysicalAddr peekRange(uintptr_t offset) = 0;

	// Returns the physical memory that backs a range of memory.
	// Ensures that the range is present before returning.
	// Result stays valid until the range is evicted.
	// TODO: This should be asynchronous.
	virtual PhysicalAddr fetchRange(uintptr_t offset) = 0;
};

struct VirtualView {
	virtual frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
	resolveRange(ptrdiff_t offset, size_t size) = 0;
};

struct CowBundle : MemoryBundle {
	CowBundle(frigg::SharedPtr<VirtualView> view, ptrdiff_t offset, size_t size);

	CowBundle(frigg::SharedPtr<CowBundle> chain, ptrdiff_t offset, size_t size);

	PhysicalAddr peekRange(uintptr_t offset) override;
	PhysicalAddr fetchRange(uintptr_t offset) override;

private:
	frigg::TicketLock _mutex;

	frigg::SharedPtr<VirtualView> _superRoot;
	frigg::SharedPtr<CowBundle> _superChain;
	ptrdiff_t _superOffset;
	frg::rcu_radixtree<std::atomic<PhysicalAddr>, KernelAlloc> _pages;
	frigg::SharedPtr<Memory> _copy;
};

struct Memory : MemoryBundle {
	static void transfer(MemoryBundle *dest_memory, uintptr_t dest_offset,
			MemoryBundle *src_memory, uintptr_t src_offset, size_t length);

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

	void submitInitiateLoad(frigg::SharedPtr<InitiateBase> initiate);
	void submitHandleLoad(frigg::SharedPtr<ManageBase> handle);
	void completeLoad(size_t offset, size_t length);

private:
	MemoryTag _tag;
};

struct CopyToBundleNode {

};

struct CopyFromBundleNode {

};

void copyToBundle(Memory *bundle, ptrdiff_t offset, const void *pointer, size_t size,
		CopyToBundleNode *node, void (*complete)(CopyToBundleNode *));

void copyFromBundle(Memory *bundle, ptrdiff_t offset, void *pointer, size_t size,
		CopyFromBundleNode *node, void (*complete)(CopyFromBundleNode *));

struct HardwareMemory : Memory {
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::hardware;
	}

	HardwareMemory(PhysicalAddr base, size_t length);
	~HardwareMemory();

	PhysicalAddr peekRange(uintptr_t offset) override;
	PhysicalAddr fetchRange(uintptr_t offset) override;

	size_t getLength();

private:
	PhysicalAddr _base;
	size_t _length;
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

	PhysicalAddr peekRange(uintptr_t offset) override;
	PhysicalAddr fetchRange(uintptr_t offset) override;
	
	size_t getLength();

private:
	frigg::TicketLock _mutex;

	frigg::Vector<PhysicalAddr, KernelAlloc> _physicalChunks;
	size_t _chunkSize, _chunkAlign;
};

struct ManagedSpace {
	enum LoadState {
		kStateMissing,
		kStateLoading,
		kStateLoaded
	};

	ManagedSpace(size_t length);
	~ManagedSpace();
	
	void progressLoads();
	bool isComplete(frigg::UnsafePtr<InitiateBase> initiate);

	frigg::TicketLock mutex;

	frigg::Vector<PhysicalAddr, KernelAlloc> physicalPages;
	frigg::Vector<LoadState, KernelAlloc> loadState;

	frigg::IntrusiveSharedLinkedList<
		InitiateBase,
		&InitiateBase::processQueueItem
	> initiateLoadQueue;

	frigg::IntrusiveSharedLinkedList<
		InitiateBase,
		&InitiateBase::processQueueItem
	> pendingLoadQueue;

	frigg::IntrusiveSharedLinkedList<
		ManageBase,
		&ManageBase::processQueueItem
	> handleLoadQueue;
};

struct BackingMemory : Memory {
public:
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::backing;
	}

	BackingMemory(frigg::SharedPtr<ManagedSpace> managed)
	: Memory(MemoryTag::backing), _managed(frigg::move(managed)) { }

	PhysicalAddr peekRange(uintptr_t offset) override;
	PhysicalAddr fetchRange(uintptr_t offset) override;

	size_t getLength();

	void submitHandleLoad(frigg::SharedPtr<ManageBase> handle);
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

	PhysicalAddr peekRange(uintptr_t offset) override;
	PhysicalAddr fetchRange(uintptr_t offset) override;

	size_t getLength();

	void submitInitiateLoad(frigg::SharedPtr<InitiateBase> initiate);

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

struct ExteriorBundleView : VirtualView {
	ExteriorBundleView(frigg::SharedPtr<MemoryBundle> bundle,
			ptrdiff_t view_offset, size_t view_size);

	frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
	resolveRange(ptrdiff_t offset, size_t size) override;

private:
	frigg::SharedPtr<MemoryBundle> _bundle;
	ptrdiff_t _viewOffset;
	size_t _viewSize;
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

	frigg::rbtree_hook treeNode;

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

	virtual frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
	resolveRange(ptrdiff_t offset, size_t size) = 0;

	virtual Mapping *shareMapping(AddressSpace *dest_space) = 0;
	virtual Mapping *copyOnWrite(AddressSpace *dest_space) = 0;

	virtual void install(bool overwrite) = 0;
	virtual void uninstall(bool clear) = 0;
	
	virtual PhysicalAddr grabPhysical(VirtualAddr disp) = 0;
	virtual bool handleFault(VirtualAddr disp, uint32_t fault_flags) = 0;

	frigg::rbtree_hook treeNode;

private:
	AddressSpace *_owner;
	VirtualAddr _address;
	size_t _length;
	MappingFlags _flags;
};

struct NormalMapping : Mapping {
	NormalMapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags, frigg::SharedPtr<VirtualView> view, uintptr_t offset);

	frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
	resolveRange(ptrdiff_t offset, size_t size) override;

	Mapping *shareMapping(AddressSpace *dest_space) override;
	Mapping *copyOnWrite(AddressSpace *dest_space) override;

	void install(bool overwrite) override;
	void uninstall(bool clear) override;

	PhysicalAddr grabPhysical(VirtualAddr disp) override;
	bool handleFault(VirtualAddr disp, uint32_t flags) override;

private:
	frigg::SharedPtr<VirtualView> _view;
	size_t _offset;
};

struct CowMapping : Mapping {
	CowMapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags, frigg::SharedPtr<CowBundle> chain);

	frigg::Tuple<MemoryBundle *, ptrdiff_t, size_t>
	resolveRange(ptrdiff_t offset, size_t size) override;

	Mapping *shareMapping(AddressSpace *dest_space) override;
	Mapping *copyOnWrite(AddressSpace *dest_space) override;

	void install(bool overwrite) override;
	void uninstall(bool clear) override;

	PhysicalAddr grabPhysical(VirtualAddr disp) override;
	bool handleFault(VirtualAddr disp, uint32_t flags) override;

private:
	frigg::SharedPtr<CowBundle> _cowBundle;
};

struct FaultNode {
	friend struct AddressSpace;

	FaultNode()
	: _resolved{false} { }

	FaultNode(const FaultNode &) = delete;

	FaultNode &operator= (const FaultNode &) = delete;

	bool resolved() {
		return _resolved;
	}

private:
	bool _resolved;
};

struct HoleLess {
	bool operator() (const Hole &a, const Hole &b) {
		return a.address() < b.address();
	}
};

struct HoleAggregator;

using HoleTree = frigg::rbtree<
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

using MappingTree = frigg::rbtree<
	Mapping,
	&Mapping::treeNode,
	MappingLess
>;

struct AddressUnmapNode {
	friend struct AddressSpace;

private:
	AddressSpace *_space;
	ShootNode _shootNode;
};

class AddressSpace {
	friend struct ForeignSpaceAccessor;

public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	typedef uint32_t MapFlags;
	enum : MapFlags {
		kMapFixed = 0x01,
		kMapPreferBottom = 0x02,
		kMapPreferTop = 0x04,
		kMapProtRead = 0x08,
		kMapProtWrite = 0x10,
		kMapProtExecute = 0x20,
		kMapDropAtFork = 0x40,
		kMapShareAtFork = 0x80,
		kMapCopyOnWriteAtFork = 0x100,
		kMapPopulate = 0x200,
		kMapDontRequireBacking = 0x400,
	};

	enum FaultFlags : uint32_t {
		kFaultWrite = (1 << 1),
		kFaultExecute = (1 << 2)
	};

	AddressSpace();

	~AddressSpace();

	void setupDefaultMappings();

	void map(Guard &guard, frigg::UnsafePtr<VirtualView> view,
			VirtualAddr address, size_t offset, size_t length,
			uint32_t flags, VirtualAddr *actual_address);
	
	void unmap(Guard &guard, VirtualAddr address, size_t length,
			AddressUnmapNode *node);

	bool handleFault(VirtualAddr address, uint32_t flags, FaultNode *node);
	
	frigg::SharedPtr<AddressSpace> fork(Guard &guard);
	
	PhysicalAddr grabPhysical(Guard &guard, VirtualAddr address);

	void activate();

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

public: // TODO: Make this private.
	ClientPageSpace _pageSpace;
};

struct AcquireNode {
	void (*acquired)();
};

struct ForeignSpaceAccessor {
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
	: _space(frigg::move(space)), _address(address), _length(length) { }

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

} // namespace thor

#endif // THOR_GENERIC_USERMEM_HPP
