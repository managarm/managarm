
namespace thor {

class Thread {
friend class ThreadQueue;
friend void switchThread(KernelUnsafePtr<Thread> thread);
public:
	enum Flags : uint32_t {
		// disables preemption for this thread
		kFlagExclusive = 1,

		// thread is not enqueued in the scheduling queue
		kFlagNotScheduled = 2
	};

	Thread(KernelSharedPtr<Universe> &&universe,
			KernelSharedPtr<AddressSpace> &&address_space,
			KernelSharedPtr<RdFolder> &&directory);

	KernelUnsafePtr<Universe> getUniverse();
	KernelUnsafePtr<AddressSpace> getAddressSpace();
	KernelUnsafePtr<RdFolder> getDirectory();

	void enableIoPort(uintptr_t port);
	
	void activate();
	void deactivate();

	ThorRtThreadState &accessSaveState();
	
	uint32_t flags;

private:
	KernelSharedPtr<Universe> p_universe;
	KernelSharedPtr<AddressSpace> p_addressSpace;
	KernelSharedPtr<RdFolder> p_directory;

	KernelSharedPtr<Thread> p_nextInQueue;
	KernelUnsafePtr<Thread> p_previousInQueue;

	ThorRtThreadState p_saveState;
	frigg::arch_x86::Tss64 p_tss;
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

