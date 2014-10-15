
namespace thor {

typedef memory::StupidMemoryAllocator KernelAllocator;

enum Error {
	kErrSuccess = 0
};

class Resource;
template<typename R>
class ResourcePtr;
class Descriptor;

typedef uint64_t Handle;

class Resource {
public:
	void install();

	Handle getResHandle();

private:
	Handle p_resHandle;
};

template<typename R>
class ResourcePtr {
public:
	Resource &operator* () {
		return *p_resource;
	}

private:
	R *p_resource;
};

class Descriptor {
public:

private:
	Handle p_descHandle;

	ResourcePtr<Resource> p_resource;
};

class ProcessResource : public Resource {
private:
};

class AddressSpaceResource : public Resource {

};

class ThreadResource : public Resource {
public:

private:
	ResourcePtr<AddressSpaceResource> p_addressSpace;
	ResourcePtr<ProcessResource> p_process;
	void *p_threadStack;
	void *p_threadIp;
};

class MemoryResource : public Resource {
public:
	MemoryResource();

	void resize(size_t length);

private:
	util::Vector<uintptr_t, KernelAllocator> p_physicalPages;
};

extern LazyInitializer<util::Vector<Resource *, KernelAllocator>> resourceMap;

} // namespace thor

