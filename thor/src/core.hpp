
namespace thor {

typedef memory::StupidMemoryAllocator KernelAllocator;

enum Error {
	kErrSuccess = 0
};

typedef uint64_t Handle;

class Descriptor {
friend class Process;
public:
	Descriptor();

	Handle getHandle();

private:
	Handle p_handle;
};

class Process : public SharedObject {
public:
	Process();

	void attachDescriptor(Descriptor *descriptor);
	Descriptor *getDescriptor(Handle handle);

private:
	util::Vector<Descriptor *, KernelAllocator> p_descriptorMap;
};

class AddressSpace : public SharedObject {
public:
	AddressSpace(memory::PageSpace page_space);

	void mapSingle4k(void *address, uintptr_t physical);

private:
	memory::PageSpace p_pageSpace;
};

class Thread : public SharedObject {
public:
	void setup(void *entry, uintptr_t argument);
	
	UnsafePtr<Process> getProcess();
	UnsafePtr<AddressSpace> getAddressSpace();
	
	void setProcess(UnsafePtr<Process> process);
	void setAddressSpace(UnsafePtr<AddressSpace> address_space);
	
	void switchTo();

	class ThreadDescriptor : public Descriptor {
	public:
		ThreadDescriptor(UnsafePtr<Thread> thread);
		
		UnsafePtr<Thread> getThread();

	private:
		SharedPtr<Thread> p_thread;
	};

private:
	SharedPtr<Process> p_process;
	SharedPtr<AddressSpace> p_addressSpace;
	ThorRtThreadState p_state;
};

class Memory : public SharedObject {
public:
	Memory();

	void resize(size_t length);

	uintptr_t getPage(int index);

	class AccessDescriptor : public Descriptor {
	public:
		AccessDescriptor(UnsafePtr<Memory> memory);

		UnsafePtr<Memory> getMemory();

	private:
		SharedPtr<Memory> p_memory;
	};
private:
	util::Vector<uintptr_t, KernelAllocator> p_physicalPages;
};

extern LazyInitializer<SharedPtr<Thread>> currentThread;

} // namespace thor

