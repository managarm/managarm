
namespace thor {

class Memory : public SharedBase<Memory, KernelAlloc> {
public:
	Memory();

	void resize(size_t length);

	uintptr_t getPage(int index);

private:
	util::Vector<uintptr_t, KernelAlloc> p_physicalPages;
};

class MemoryAccessDescriptor {
public:
	MemoryAccessDescriptor(SharedPtr<Memory, KernelAlloc> &&memory);

	UnsafePtr<Memory, KernelAlloc> getMemory();

private:
	SharedPtr<Memory, KernelAlloc> p_memory;
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

class AddressSpace : public SharedBase<AddressSpace, KernelAlloc> {
public:
	AddressSpace(memory::PageSpace page_space);

	Mapping *getMapping(VirtualAddr address);
	
	// allocates a new mapping of the given length somewhere in the address space
	// the new mapping has type kTypeNone
	Mapping *allocate(size_t length);

	Mapping *allocateAt(VirtualAddr address, size_t length);

	// creates a new mapping inside a hole
	// the new mapping has type kTypeNone
	Mapping *splitHole(Mapping *mapping,
			VirtualAddr split_offset, VirtualAddr split_length);

	void mapSingle4k(void *address, uintptr_t physical);

private:
	Mapping *allocateDfs(Mapping *mapping, size_t length);

	// Left rotation (n denotes the given mapping):
	//   w                 w
	//   |                 |
	//   u                 n
	//  / \      -->      / \
	// x   n             u   y
	//    / \           / \
	//   v   y         x   v
	// Note that x and y are left unchanged
	void rotateLeft(Mapping *mapping);

	// Right rotation (n denotes the given mapping):
	//     w             w
	//     |             |
	//     u             n
	//    / \    -->    / \
	//   n   x         y   u
	//  / \               / \
	// y   v             v   x
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
	memory::PageSpace p_pageSpace;
};

} // namespace thor

