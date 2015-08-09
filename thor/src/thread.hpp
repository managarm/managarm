
namespace thor {

class Thread {
friend class ThreadQueue;
friend void switchThread(UnsafePtr<Thread, KernelAlloc> thread);
public:
	Thread(SharedPtr<Universe, KernelAlloc> &&universe,
			SharedPtr<AddressSpace, KernelAlloc> &&address_space,
			SharedPtr<RdFolder, KernelAlloc> &&directory,
			bool kernel_thread);

	void setup(void (*user_entry)(uintptr_t), uintptr_t argument,
			void *user_stack_ptr);
	
	UnsafePtr<Universe, KernelAlloc> getUniverse();
	UnsafePtr<AddressSpace, KernelAlloc> getAddressSpace();
	UnsafePtr<RdFolder, KernelAlloc> getDirectory();

	bool isKernelThread();

	void enableIoPort(uintptr_t port);

private:
	SharedPtr<Universe, KernelAlloc> p_universe;
	SharedPtr<AddressSpace, KernelAlloc> p_addressSpace;
	SharedPtr<RdFolder, KernelAlloc> p_directory;

	SharedPtr<Thread, KernelAlloc> p_nextInQueue;
	UnsafePtr<Thread, KernelAlloc> p_previousInQueue;

	ThorRtThreadState p_state;
	frigg::arch_x86::Tss64 p_tss;
	bool p_kernelThread;
};

void switchThread(UnsafePtr<Thread, KernelAlloc> thread);

class ThreadQueue {
public:
	ThreadQueue();

	bool empty();

	void addBack(SharedPtr<Thread, KernelAlloc> &&thread);
	
	SharedPtr<Thread, KernelAlloc> removeFront();
	SharedPtr<Thread, KernelAlloc> remove(UnsafePtr<Thread, KernelAlloc> thread);

private:
	SharedPtr<Thread, KernelAlloc> p_front;
	UnsafePtr<Thread, KernelAlloc> p_back;
};

} // namespace thor

