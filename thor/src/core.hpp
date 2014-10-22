
namespace thor {

typedef memory::StupidMemoryAllocator KernelAllocator;

enum Error {
	kErrSuccess = 0
};

class Resource;
class Descriptor;

typedef uint64_t Handle;

class Resource : public SharedObject {
public:
	Handle getResHandle();

	void install();

private:
	Handle p_resHandle;
};


class ProcessResource : public Resource {
private:
};

class AddressSpaceResource : public Resource {
public:
	AddressSpaceResource(memory::PageSpace page_space);

	void mapSingle4k(void *address, uintptr_t physical);

private:
	memory::PageSpace p_pageSpace;
};

class ThreadResource : public Resource {
friend void switchTo(const SharedPtr<ThreadResource> &thread_res);
public:
	void setup(void *entry, uintptr_t argument);

	SharedPtr<AddressSpaceResource> getAddressSpace();

	void setAddressSpace(SharedPtr<AddressSpaceResource> address_space);
	
	void switchTo();

private:
	SharedPtr<AddressSpaceResource> p_addressSpace;
	ThorRtThreadState p_state;
};

class MemoryResource : public Resource {
public:
	MemoryResource();

	void resize(size_t length);

	uintptr_t getPage(int index);

private:
	util::Vector<uintptr_t, KernelAllocator> p_physicalPages;
};

class Descriptor {
public:

private:
	Handle p_descHandle;

	SharedPtr<Resource> p_resource;
};


extern LazyInitializer<util::Vector<UnsafePtr<Resource>, KernelAllocator>> resourceMap;

extern LazyInitializer<SharedPtr<ThreadResource>> currentThread;

} // namespace thor

