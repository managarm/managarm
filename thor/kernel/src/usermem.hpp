
namespace thor {

class Memory {
public:
	enum Type {
		kTypeNone,
		kTypePhysical,
		kTypeAllocated,
		kTypeCopyOnWrite
	};

	enum Flags : uint32_t {
		kFlagOnDemand = 0x01
	};

	Memory(Type type);
	~Memory();

	Type getType();

	void resize(size_t length);

	void setPage(size_t index, PhysicalAddr page);
	PhysicalAddr getPage(size_t index);

	PhysicalAddr resolveOriginal(size_t index);

	size_t numPages();
	
	void zeroPages();
	void copyTo(size_t offset, void *source, size_t length);

	uint32_t flags;

	KernelSharedPtr<Memory> master;

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
		kMapShareOnFork = 0x40
	};

	enum FaultFlags : uint32_t {
		kFaultWrite = 0x01
	};

	AddressSpace(PageSpace page_space);

	~AddressSpace();

	void setupDefaultMappings();

	void map(Guard &guard, KernelUnsafePtr<Memory> memory,
			VirtualAddr address, size_t length,
			uint32_t flags, VirtualAddr *actual_address);
	
	void unmap(Guard &guard, VirtualAddr address, size_t length);

	bool handleFault(Guard &guard, VirtualAddr address, uint32_t flags);
	
	KernelSharedPtr<AddressSpace> fork(Guard &guard);
	
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

