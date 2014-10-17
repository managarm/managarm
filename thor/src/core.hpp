
namespace thor {

typedef memory::StupidMemoryAllocator KernelAllocator;

enum Error {
	kErrSuccess = 0
};

class Resource;
class Descriptor;

typedef uint64_t Handle;

class Resource {
public:
	void install();

	Handle getResHandle();

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
public:
	RefCountPtr<AddressSpaceResource> getAddressSpace();

	void setAddressSpace(RefCountPtr<AddressSpaceResource> address_space);

private:
	RefCountPtr<AddressSpaceResource> p_addressSpace;
	void *p_threadStack;
	void *p_threadIp;
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

	RefCountPtr<Resource> p_resource;
};


extern LazyInitializer<util::Vector<Resource *, KernelAllocator>> resourceMap;

extern LazyInitializer<RefCountPtr<ThreadResource>> currentThread;

} // namespace thor

