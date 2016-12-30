
#include <frigg/rbtree.hpp>
#include "futex.hpp"

namespace thor {

struct Memory;

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

struct Memory {
	static void transfer(frigg::UnsafePtr<Memory> dest_memory, uintptr_t dest_offset,
			frigg::UnsafePtr<Memory> src_memory, uintptr_t src_offset, size_t length);

	Memory(MemoryTag tag)
	: _tag(tag) { }

	Memory(const Memory &) = delete;

	Memory &operator= (const Memory &) = delete;
	
	MemoryTag tag() const {
		return _tag;
	}

	size_t getLength();

	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			GrabIntent grab_flags, size_t offset);
	
	void submitInitiateLoad(frigg::SharedPtr<InitiateBase> initiate);
	void submitHandleLoad(frigg::SharedPtr<ManageBase> handle);
	void completeLoad(size_t offset, size_t length);

	void load(size_t offset, void *pointer, size_t length);
	void copyFrom(size_t offset, void *pointer, size_t length);

	Futex futex;

private:
	MemoryTag _tag;
};

struct HardwareMemory : Memory {
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::hardware;
	}

	HardwareMemory(PhysicalAddr base, size_t length);
	~HardwareMemory();

	size_t getLength();

	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			GrabIntent grab_intent, size_t offset);

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
	
	size_t getLength();

	// TODO: add a method to populate the memory
	
	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			GrabIntent grab_intent, size_t offset);

private:
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

	size_t getLength();

	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			GrabIntent grab_intent, size_t offset);
	
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

	size_t getLength();

	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			GrabIntent grab_intent, size_t offset);
	
	void submitInitiateLoad(frigg::SharedPtr<InitiateBase> initiate);

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

struct CopyOnWriteMemory : Memory {
	static bool classOf(const Memory &memory) {
		return memory.tag() == MemoryTag::copyOnWrite;
	}

	CopyOnWriteMemory(frigg::SharedPtr<Memory> origin);
	~CopyOnWriteMemory();

	size_t getLength();

	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			GrabIntent grab_intent, size_t offset);

private:
	frigg::SharedPtr<Memory> _origin;

	frigg::Vector<PhysicalAddr, KernelAlloc> _physicalPages;
};

struct Mapping;

struct Mapping {
	enum Type {
		kTypeNone,
		kTypeHole,
		kTypeMemory
	};

	enum Flags : uint32_t {
		kFlagDropAtFork = 0x01,
		kFlagShareAtFork = 0x02,
		kFlagCopyOnWriteAtFork = 0x04,
		kFlagDontRequireBacking = 0x08
	};

	Mapping(Type type, VirtualAddr base_address, size_t length);

	~Mapping();

	VirtualAddr baseAddress;
	size_t length;
	Type type;
	
	// larget hole in the subtree of this node
	size_t largestHole;

	KernelSharedPtr<Memory> memoryRegion;
	size_t memoryOffset;
	uint32_t flags;
	bool writePermission, executePermission;

	frigg::rbtree_hook spaceHook;
};

struct SpaceLess {
	bool operator() (const Mapping &a, const Mapping &b) {
		return a.baseAddress < b.baseAddress;
	}
};

struct SpaceAggregator;

using SpaceTree = frigg::rbtree<
	Mapping,
	&Mapping::spaceHook,
	SpaceLess,
	SpaceAggregator
>;

struct SpaceAggregator {
	static bool aggregate(Mapping *node);
	static bool check_invariant(SpaceTree &tree, Mapping *node);
};

class AddressSpace {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	typedef uint32_t MapFlags;
	enum : MapFlags {
		kMapFixed = 0x01,
		kMapPreferBottom = 0x02,
		kMapPreferTop = 0x04,
		kMapReadOnly = 0x08,
		kMapReadWrite = 0x10,
		kMapReadExecute = 0x20,
		kMapDropAtFork = 0x40,
		kMapShareAtFork = 0x80,
		kMapCopyOnWriteAtFork = 0x100,
		kMapPopulate = 0x200,
		kMapDontRequireBacking = 0x400,
	};

	enum FaultFlags : uint32_t {
		kFaultWrite = 0x01
	};

	AddressSpace(PageSpace page_space);

	~AddressSpace();

	void setupDefaultMappings();

	void map(Guard &guard, KernelUnsafePtr<Memory> memory,
			VirtualAddr address, size_t offset, size_t length,
			uint32_t flags, VirtualAddr *actual_address);
	
	void unmap(Guard &guard, VirtualAddr address, size_t length);

	bool handleFault(Guard &guard, VirtualAddr address, uint32_t flags);
	
	KernelSharedPtr<AddressSpace> fork(Guard &guard);
	
	PhysicalAddr grabPhysical(Guard &guard, VirtualAddr address);

	void activate();
	
	// TODO: mappings should be referenced by shared pointers.
	Mapping *getMapping(VirtualAddr address);

	Lock lock;
	
	QueueSpace queueSpace;

private:
	
	// allocates a new mapping of the given length somewhere in the address space
	// the new mapping has type kTypeNone
	Mapping *allocate(size_t length, MapFlags flags);

	Mapping *allocateAt(VirtualAddr address, size_t length);

	void cloneRecursive(Mapping *mapping, AddressSpace *dest_space);

	// creates a new mapping inside a hole
	// the new mapping has type kTypeNone
	Mapping *splitHole(Mapping *mapping,
			VirtualAddr split_offset, VirtualAddr split_length);
	
	Mapping *allocateDfs(Mapping *mapping, size_t length, MapFlags flags);
	
	SpaceTree spaceTree;

	PageSpace p_pageSpace;
};

} // namespace thor

