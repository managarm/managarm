
namespace thor {

class Memory : public SharedObject {
public:
	Memory();

	void resize(size_t length);

	uintptr_t getPage(int index);

private:
	util::Vector<uintptr_t, KernelAlloc> p_physicalPages;
};

class MemoryAccessDescriptor {
public:
	MemoryAccessDescriptor(SharedPtr<Memory> &&memory);

	UnsafePtr<Memory> getMemory();

private:
	SharedPtr<Memory> p_memory;
};

struct Mapping {
	enum Type {
		kTypeNone,
		kTypeHole,
		kTypeMemory
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
	
	// larget hole in the subtree of this node
	size_t largestHole;

	Mapping(Type type, VirtualAddr base_address, size_t length);
};

class AddressSpace : public SharedObject {
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

	void dumpMappingsDfs(Mapping *mapping);

	void addressTreeInsert(Mapping *mapping);
	void addressTreeRemove(Mapping *mapping);

	void updateLargestHole(Mapping *mapping);
	
	Mapping *p_root;
	memory::PageSpace p_pageSpace;
};

} // namespace thor

