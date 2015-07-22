
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

class AddressSpace : public SharedObject {
public:
	AddressSpace(memory::PageSpace page_space);

	void mapSingle4k(void *address, uintptr_t physical);

private:
	memory::PageSpace p_pageSpace;
};

} // namespace thor

