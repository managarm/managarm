
namespace thor {

class Thread {
friend class ThreadQueue;
friend void switchThread(KernelUnsafePtr<Thread> thread);
public:
	Thread(KernelSharedPtr<Universe> &&universe,
			KernelSharedPtr<AddressSpace> &&address_space,
			KernelSharedPtr<RdFolder> &&directory,
			bool kernel_thread);

	void setup(void (*user_entry)(uintptr_t), uintptr_t argument,
			void *user_stack_ptr);
	
	KernelUnsafePtr<Universe> getUniverse();
	KernelUnsafePtr<AddressSpace> getAddressSpace();
	KernelUnsafePtr<RdFolder> getDirectory();

	bool isKernelThread();

	void enableIoPort(uintptr_t port);
	
	void activate();
	void deactivate();

	ThorRtThreadState &accessSaveState();

private:
	KernelSharedPtr<Universe> p_universe;
	KernelSharedPtr<AddressSpace> p_addressSpace;
	KernelSharedPtr<RdFolder> p_directory;

	KernelSharedPtr<Thread> p_nextInQueue;
	KernelUnsafePtr<Thread> p_previousInQueue;

	ThorRtThreadState p_saveState;
	frigg::arch_x86::Tss64 p_tss;
	bool p_kernelThread;
};

class ThreadQueue {
public:
	ThreadQueue();

	bool empty();

	void addBack(KernelSharedPtr<Thread> &&thread);
	
	KernelSharedPtr<Thread> removeFront();
	KernelSharedPtr<Thread> remove(KernelUnsafePtr<Thread> thread);

private:
	KernelSharedPtr<Thread> p_front;
	KernelUnsafePtr<Thread> p_back;
};

} // namespace thor

