
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

	PhysicalAddr grabPage(GrabIntent grab_flags, size_t offset);
	
	void submitInitiateLoad(frigg::SharedPtr<InitiateBase> initiate);
	void submitHandleLoad(frigg::SharedPtr<ManageBase> handle);
	void completeLoad(size_t offset, size_t length);

	void load(size_t offset, void *pointer, size_t length);
	void copyFrom(size_t offset, void *pointer, size_t length);

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

	PhysicalAddr grabPage(GrabIntent grab_intent, size_t offset);

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
	
	PhysicalAddr grabPage(GrabIntent grab_intent, size_t offset);

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

	PhysicalAddr grabPage(GrabIntent grab_intent, size_t offset);
	
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

	PhysicalAddr grabPage(GrabIntent grab_intent, size_t offset);
	
	void submitInitiateLoad(frigg::SharedPtr<InitiateBase> initiate);

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

enum class MappingType {
	null, hole, other
};

enum MappingFlags : uint32_t {
	null = 0,

	forkMask = 0x07,
	dropAtFork = 0x01,
	shareAtFork = 0x02,
	copyOnWriteAtFork = 0x04,

	permissionMask = 0x30,
	readOnly = 0x00,
	readWrite = 0x10,
	readExecute = 0x20,

	dontRequireBacking = 0x100
};

struct CowChain {
	CowChain()
	: mask(*kernelAlloc) { }

	frigg::SharedPtr<Memory> memory;
	VirtualAddr offset;
	frigg::SharedPtr<CowChain> super;
	frigg::Vector<bool, KernelAlloc> mask;
};

struct Mapping {
	Mapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags);

	virtual MappingType type() = 0;

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

	virtual Mapping *shareMapping(AddressSpace *dest_space) = 0;
	virtual Mapping *copyMapping(AddressSpace *dest_space) = 0;
	virtual Mapping *copyOnWrite(AddressSpace *dest_space) = 0;

	virtual void install(bool overwrite) = 0;
	virtual void uninstall(bool clear) = 0;
	
	virtual PhysicalAddr grabPhysical(VirtualAddr disp) = 0;
	virtual bool handleFault(VirtualAddr disp, uint32_t flags) = 0;

	frigg::rbtree_hook spaceHook;

private:
	AddressSpace *_owner;
	VirtualAddr _address;
	size_t _length;
	MappingFlags _flags;

public:
	// larget hole in the subtree of this node
	size_t largestHole;
};

struct HoleMapping : Mapping {
	HoleMapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags);

	MappingType type() override {
		return MappingType::hole;
	}

	Mapping *shareMapping(AddressSpace *dest_space) override;
	Mapping *copyMapping(AddressSpace *dest_space) override;
	Mapping *copyOnWrite(AddressSpace *dest_space) override;

	void install(bool overwrite) override;
	void uninstall(bool clear) override;
	
	PhysicalAddr grabPhysical(VirtualAddr disp) override;
	bool handleFault(VirtualAddr disp, uint32_t flags) override;
};

struct NormalMapping : Mapping {
	NormalMapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags, frigg::SharedPtr<Memory> memory, uintptr_t offset);
	~NormalMapping();

	MappingType type() override {
		return MappingType::other;
	}

	Mapping *shareMapping(AddressSpace *dest_space) override;
	Mapping *copyMapping(AddressSpace *dest_space) override;
	Mapping *copyOnWrite(AddressSpace *dest_space) override;

	void install(bool overwrite) override;
	void uninstall(bool clear) override;

	PhysicalAddr grabPhysical(VirtualAddr disp) override;
	bool handleFault(VirtualAddr disp, uint32_t flags) override;

private:
	KernelSharedPtr<Memory> _memory;
	size_t _offset;
};

struct CowMapping : Mapping {
	CowMapping(AddressSpace *owner, VirtualAddr address, size_t length,
			MappingFlags flags, frigg::SharedPtr<CowChain> chain);
	~CowMapping();

	MappingType type() override {
		return MappingType::other;
	}

	Mapping *shareMapping(AddressSpace *dest_space) override;
	Mapping *copyMapping(AddressSpace *dest_space) override;
	Mapping *copyOnWrite(AddressSpace *dest_space) override;

	void install(bool overwrite) override;
	void uninstall(bool clear) override;

	PhysicalAddr grabPhysical(VirtualAddr disp) override;
	bool handleFault(VirtualAddr disp, uint32_t flags) override;

private:
	PhysicalAddr _retrievePage(VirtualAddr disp);

	frigg::SharedPtr<Memory> _copy;
	frigg::Vector<bool, KernelAlloc> _mask;
	frigg::SharedPtr<CowChain> _chain;
};

struct SpaceLess {
	bool operator() (const Mapping &a, const Mapping &b) {
		return a.address() < b.address();
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
	
	Futex futexSpace;
	QueueSpace queueSpace;

private:
	
	// Allocates a new mapping of the given length somewhere in the address space.
	VirtualAddr _allocate(size_t length, MapFlags flags);

	VirtualAddr _allocateAt(VirtualAddr address, size_t length);

	void cloneRecursive(Mapping *mapping, AddressSpace *dest_space);

	// Splits some memory range from a hole mapping.
	void splitHole(Mapping *mapping, VirtualAddr offset, VirtualAddr length);
	
	VirtualAddr _allocateDfs(Mapping *mapping, size_t length, MapFlags flags);
	
	SpaceTree spaceTree;

public: // TODO: Make this private.
	PageSpace p_pageSpace;
};

} // namespace thor

