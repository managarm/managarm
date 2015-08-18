
namespace thor {

class Memory {
public:
	Memory();

	void resize(size_t length);
	void addPage(PhysicalAddr page);

	PhysicalAddr getPage(int index);

	size_t getSize();

private:
	frigg::util::Vector<PhysicalAddr, KernelAlloc> p_physicalPages;
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

	VirtualAddr baseAddress;
	size_t length;
	Type type;
	
	// pointers to the next / previous mapping in virtual memory
	Mapping *lowerPtr;
	Mapping *higherPtr;
	
	// pointers to the next / previous / parent mappings in the address tree
	Mapping *leftPtr;
	Mapping *rightPtr;
	Mapping *parentPtr;
	Color color;
	
	// larget hole in the subtree of this node
	size_t largestHole;

	SharedPtr<Memory, KernelAlloc> memoryRegion;
	size_t memoryOffset;

	Mapping(Type type, VirtualAddr base_address, size_t length);
};

class AddressSpace {
public:
	typedef uint32_t MapFlags;
	enum : MapFlags {
		kMapFixed = 0x01,
		kMapPreferBottom = 0x02,
		kMapPreferTop = 0x04,
		kMapReadOnly = 0x08,
		kMapReadWrite = 0x10,
		kMapReadExecute = 0x20
	};

	AddressSpace(PageSpace page_space);

	void map(UnsafePtr<Memory, KernelAlloc> memory, VirtualAddr address, size_t length,
			uint32_t flags, VirtualAddr *actual_address);
	
	void activate();

private:
	Mapping *getMapping(VirtualAddr address);
	
	// allocates a new mapping of the given length somewhere in the address space
	// the new mapping has type kTypeNone
	Mapping *allocate(size_t length, MapFlags flags);

	Mapping *allocateAt(VirtualAddr address, size_t length);

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
	void rotateLeft(Mapping *mapping);

	// Right rotation (n denotes the given mapping):
	//     w             w          |
	//     |             |          |
	//     u             n          |
	//    / \    -->    / \         |
	//   n   x         y   u        |
	//  / \               / \       |
	// y   v             v   x      |
	// Note that x and y are left unchanged
	void rotateRight(Mapping *mapping);

	bool isRed(Mapping *mapping);
	bool isBlack(Mapping *mapping);

	void addressTreeInsert(Mapping *mapping);
	void fixAfterInsert(Mapping *mapping);

	void addressTreeRemove(Mapping *mapping);
	void fixAfterRemove(Mapping *mapping);

	void updateLargestHole(Mapping *mapping);
	
	Mapping *p_root;
	PageSpace p_pageSpace;
};

} // namespace thor

