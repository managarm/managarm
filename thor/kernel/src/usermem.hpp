
namespace thor {

class Memory {
public:
	enum Type {
		kTypeNone,
		kTypePhysical,
		kTypeAllocated,
		kTypeOnDemand,
		kTypeBacked,
		kTypeCopyOnWrite
	};

	enum Flags : uint32_t {
	
	};
	
	// page state for kTypeBacked regions
	enum LoadState {
		kStateMissing,
		kStateLoading,
		kStateLoaded
	};

	struct ProcessRequest {
		ProcessRequest(frigg::SharedPtr<EventHub> event_hub, SubmitInfo submit_info);
		
		frigg::SharedPtr<EventHub> eventHub;
		SubmitInfo submitInfo;
	};

	struct LoadOrder {
		LoadOrder(uintptr_t offset, size_t size);
		
		uintptr_t offset;
		size_t size;
	};

	struct LockRequest {
		LockRequest(uintptr_t offset, size_t size,
				frigg::SharedPtr<EventHub> event_hub, SubmitInfo submit_info);
		
		uintptr_t offset;
		size_t size;
		frigg::SharedPtr<EventHub> eventHub;
		SubmitInfo submitInfo;
	};

	Memory(Type type);
	~Memory();

	Type getType();

	void resize(size_t length);

	void setPageAt(size_t offset, PhysicalAddr page);
	PhysicalAddr getPageAt(size_t offset);

	PhysicalAddr resolveOriginalAt(size_t offset);

	PhysicalAddr grabPage(PhysicalChunkAllocator::Guard &physical_guard,
			size_t offset);

	size_t numPages();
	
	void zeroPages();
	void copyTo(size_t offset, void *source, size_t length);

	// submits a load request for a certain chunk of memory
	void loadMemory(uintptr_t offset, size_t size);

	// raises an event for the ProcessRequest
	void performLoad(ProcessRequest *process_request, LoadOrder *load_order);

	bool checkLock(LockRequest *lock_request);

	// raises an event for the LockRequest
	void performLock(LockRequest *lock_request);

	uint32_t flags;

	KernelSharedPtr<Memory> master;

	// TODO: make this private?
	frigg::Vector<LoadState, KernelAlloc> loadState;
	
	frigg::LinkedList<ProcessRequest, KernelAlloc> processQueue;
	
	frigg::LinkedList<LoadOrder, KernelAlloc> loadQueue;
	
	frigg::LinkedList<LockRequest, KernelAlloc> lockQueue;

	// threads blocking until a load request is finished
	frigg::LinkedList<frigg::SharedPtr<Thread>, KernelAlloc> waitQueue;

private:
	Type p_type;
	frigg::Vector<PhysicalAddr, KernelAlloc> p_physicalPages;
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
		kFlagShareOnFork = 0x01
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
		kMapShareOnFork = 0x40,
		kMapBacking = 0x80
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
	
	PhysicalAddr getPhysical(Guard &guard, VirtualAddr address);
	
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

// directly accesses an object in an arbitrary address space.
// requires the object's address to be naturally aligned
// so that the object cannot cross a page boundary.
// requires the object to be smaller than a page for the same reason.
template<typename T>
struct DirectSpaceLock {
	static DirectSpaceLock acquire(frigg::SharedPtr<AddressSpace> space, T *address) {
		assert(sizeof(T) <= kPageSize);
		assert((VirtualAddr)address % sizeof(T) == 0);
		// TODO: actually lock the memory + make sure the memory is mapped as writeable
		// TODO: return an empty lock if the acquire fails
		return DirectSpaceLock(frigg::move(space), address);
	}

	friend void swap(DirectSpaceLock &a, DirectSpaceLock &b) {
		frigg::swap(a._space, b._space);
		frigg::swap(a._address, b._address);
	}

	DirectSpaceLock() = default;

	DirectSpaceLock(const DirectSpaceLock &other) = delete;

	DirectSpaceLock(DirectSpaceLock &&other)
	: DirectSpaceLock() {
		swap(*this, other);
	}
	
	DirectSpaceLock &operator= (DirectSpaceLock other) {
		swap(*this, other);
		return *this;
	}
	
	frigg::UnsafePtr<AddressSpace> space() {
		return _space;
	}
	void *foreignAddress() {
		return _address;
	}

	T *get() {
		assert(_space);
		size_t misalign = (VirtualAddr)_address % kPageSize;
		AddressSpace::Guard guard(&_space->lock);
		PhysicalAddr page = _space->grabPhysical(guard, (VirtualAddr)_address - misalign);
		return reinterpret_cast<T *>(physicalToVirtual(page + misalign));
	}

	T &operator* () {
		return *get();
	}
	T *operator-> () {
		return get();
	}

private:
	DirectSpaceLock(frigg::SharedPtr<AddressSpace> space, T *address)
	: _space(frigg::move(space)), _address(address) { }

	frigg::SharedPtr<AddressSpace> _space;
	void *_address;
};

struct ForeignSpaceLock {
	static ForeignSpaceLock acquire(frigg::SharedPtr<AddressSpace> space,
			void *address, size_t length) {
		// TODO: actually lock the memory + make sure the memory is mapped as writeable
		// TODO: return an empty lock if the acquire fails
		return ForeignSpaceLock(frigg::move(space), address, length);
	}

	friend void swap(ForeignSpaceLock &a, ForeignSpaceLock &b) {
		frigg::swap(a._space, b._space);
		frigg::swap(a._address, b._address);
		frigg::swap(a._length, b._length);
	}

	ForeignSpaceLock() = default;

	ForeignSpaceLock(const ForeignSpaceLock &other) = delete;

	ForeignSpaceLock(ForeignSpaceLock &&other)
	: ForeignSpaceLock() {
		swap(*this, other);
	}
	
	ForeignSpaceLock &operator= (ForeignSpaceLock other) {
		swap(*this, other);
		return *this;
	}

	frigg::UnsafePtr<AddressSpace> space() {
		return _space;
	}
	size_t length() {
		return _length;
	}

	void copyTo(void *pointer, size_t size) {
		AddressSpace::Guard guard(&_space->lock);
		
		size_t offset = 0;
		while(offset < size) {
			VirtualAddr write = (VirtualAddr)_address + offset;
			size_t misalign = (VirtualAddr)write % kPageSize;
			size_t chunk = frigg::min(kPageSize - misalign, size - offset);

			PhysicalAddr page = _space->grabPhysical(guard, write - misalign);
			memcpy(physicalToVirtual(page + misalign), (char *)pointer + offset, chunk);
			offset += chunk;
		}
	}

private:
	ForeignSpaceLock(frigg::SharedPtr<AddressSpace> space,
			void *address, size_t length)
	: _space(frigg::move(space)), _address(address), _length(length) { }

	frigg::SharedPtr<AddressSpace> _space;
	void *_address;
	size_t _length;
};

struct OwnSpaceLock {

};

} // namespace thor

