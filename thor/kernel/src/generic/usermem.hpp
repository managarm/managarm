
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

struct HardwareMemory {
	HardwareMemory(PhysicalAddr base, size_t length);
	
	size_t getLength();

	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			GrabIntent grab_intent, size_t offset);

private:
	PhysicalAddr _base;
	size_t _length;
};

struct AllocatedMemory {
	AllocatedMemory(size_t length, size_t chunk_size = kPageSize,
			size_t chunk_align = kPageSize);
	
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
	
	void progressLoads();
	bool isComplete(frigg::UnsafePtr<AsyncInitiateLoad> initiate);

	frigg::Vector<PhysicalAddr, KernelAlloc> physicalPages;
	frigg::Vector<LoadState, KernelAlloc> loadState;

	frigg::IntrusiveSharedLinkedList<
		AsyncInitiateLoad,
		&AsyncInitiateLoad::processQueueItem
	> initiateLoadQueue;

	frigg::IntrusiveSharedLinkedList<
		AsyncInitiateLoad,
		&AsyncInitiateLoad::processQueueItem
	> pendingLoadQueue;

	frigg::IntrusiveSharedLinkedList<
		AsyncHandleLoad,
		&AsyncHandleLoad::processQueueItem
	> handleLoadQueue;
};

struct BackingMemory {
public:
	BackingMemory(frigg::SharedPtr<ManagedSpace> managed)
	: _managed(frigg::move(managed)) { }

	size_t getLength();

	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			GrabIntent grab_intent, size_t offset);
	
	void submitHandleLoad(frigg::SharedPtr<AsyncHandleLoad> handle);
	void completeLoad(size_t offset, size_t length);

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

struct FrontalMemory {
public:
	FrontalMemory(frigg::SharedPtr<ManagedSpace> managed)
	: _managed(frigg::move(managed)) { }

	size_t getLength();

	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			GrabIntent grab_intent, size_t offset);
	
	void submitInitiateLoad(frigg::SharedPtr<AsyncInitiateLoad> initiate);

private:
	frigg::SharedPtr<ManagedSpace> _managed;
};

struct CopyOnWriteMemory {
	CopyOnWriteMemory(frigg::SharedPtr<Memory> origin);

	size_t getLength();

	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			GrabIntent grab_intent, size_t offset);

private:
	frigg::SharedPtr<Memory> _origin;

	frigg::Vector<PhysicalAddr, KernelAlloc> _physicalPages;
};

struct Memory {
	typedef frigg::Variant<
		HardwareMemory,
		AllocatedMemory,
		BackingMemory,
		FrontalMemory,
		CopyOnWriteMemory
	> MemoryVariant;

	Memory(MemoryVariant variant);
		
	size_t getLength() {
		return _variant.apply([&] (auto &unboxed) {
			return unboxed.getLength();
		});
	}

	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			GrabIntent grab_flags, size_t offset) {
		assert((grab_flags & kGrabQuery) || (grab_flags & kGrabFetch));
		assert(!((grab_flags & kGrabQuery) && (grab_flags & kGrabFetch)));
		return _variant.apply([&] (auto &unboxed) {
			return unboxed.grabPage(physical_guard, grab_flags, offset);
		});
	}
	
	void submitInitiateLoad(frigg::SharedPtr<AsyncInitiateLoad> initiate) {
		switch(_variant.tag()) {
		case MemoryVariant::tagOf<FrontalMemory>():
			_variant.get<FrontalMemory>().submitInitiateLoad(frigg::move(initiate));
			break;
		case MemoryVariant::tagOf<HardwareMemory>():
		case MemoryVariant::tagOf<AllocatedMemory>():
			AsyncOperation::complete(frigg::move(initiate));
			break;
		case MemoryVariant::tagOf<CopyOnWriteMemory>():
			assert(!"Not implemented yet");
		default:
			assert(!"Not supported");
		}
	}

	void submitHandleLoad(frigg::SharedPtr<AsyncHandleLoad> handle) {
		switch(_variant.tag()) {
		case MemoryVariant::tagOf<BackingMemory>():
			_variant.get<BackingMemory>().submitHandleLoad(frigg::move(handle));
			break;
		default:
			assert(!"Not supported");
		}
	}
	void completeLoad(size_t offset, size_t length) {
		switch(_variant.tag()) {
		case MemoryVariant::tagOf<BackingMemory>():
			_variant.get<BackingMemory>().completeLoad(offset, length);
			break;
		default:
			assert(!"Not supported");
		}
	}

	void copyFrom(size_t offset, void *pointer, size_t length);

private:
	MemoryVariant _variant;
};

struct Mapping {
	enum Type {
		kTypeNone,
		kTypeHole,
		kTypeMemory
	};


	enum Color {
		kColorNone,
		kColorRed,
		kColorBlack
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
	
	// pointers to the next / previous mapping in virtual memory
	Mapping *lowerPtr;
	Mapping *higherPtr;
	
	// pointers to the left / right / parent mappings in the address tree
	Mapping *leftPtr;
	Mapping *rightPtr;
	Mapping *parentPtr;
	Color color;
	
	// larget hole in the subtree of this node
	size_t largestHole;

	KernelSharedPtr<Memory> memoryRegion;
	size_t memoryOffset;
	uint32_t flags;
	bool writePermission, executePermission;
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

	Lock lock;

private:
	Mapping *getMapping(VirtualAddr address);
	
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

	// Left rotation (n denotes the given mapping):
	//   w                 w        |
	//   |                 |        |
	//   u                 n        |
	//  / \      -->      / \       |
	// x   n             u   y      |
	//    / \           / \         |
	//   v   y         x   v        |
	// Note that x and y are left unchanged
	void rotateLeft(Mapping *n);

	// Right rotation (n denotes the given mapping):
	//     w             w          |
	//     |             |          |
	//     u             n          |
	//    / \    -->    / \         |
	//   n   x         y   u        |
	//  / \               / \       |
	// y   v             v   x      |
	// Note that x and y are left unchanged
	void rotateRight(Mapping *n);

	bool isRed(Mapping *mapping);
	bool isBlack(Mapping *mapping);

	void addressTreeInsert(Mapping *mapping);
	void fixAfterInsert(Mapping *mapping);

	void addressTreeRemove(Mapping *mapping);
	void replaceNode(Mapping *node, Mapping *replacement);
	void removeHalfLeaf(Mapping *mapping, Mapping *child);
	void fixAfterRemove(Mapping *mapping);
	
	bool checkInvariant();
	bool checkInvariant(Mapping *mapping, int &black_depth,
			Mapping *&minimum, Mapping *&maximum);

	bool updateLargestHoleAt(Mapping *mapping);
	void updateLargestHoleUpwards(Mapping *mapping);
	
	Mapping *p_root;
	PageSpace p_pageSpace;
};

} // namespace thor

