
namespace thor {

class ThreadGroup;

class Thread : public PlatformExecutor {
friend class ThreadQueue;
public:
	enum Flags : uint32_t {
		// disables preemption for this thread
		kFlagExclusive = 1,

		// thread is not enqueued in the scheduling queue
		// e.g. this is set for the per-cpu idle threads
		kFlagNotScheduled = 2
	};

	Thread(KernelSharedPtr<Universe> &&universe,
			KernelSharedPtr<AddressSpace> &&address_space,
			KernelSharedPtr<RdFolder> &&directory);
	~Thread();

	void setThreadGroup(KernelSharedPtr<ThreadGroup> group);
	
	KernelUnsafePtr<ThreadGroup> getThreadGroup();
	KernelUnsafePtr<Universe> getUniverse();
	KernelUnsafePtr<AddressSpace> getAddressSpace();
	KernelUnsafePtr<RdFolder> getDirectory();

	void queueSignal(void *entry);

	void issueSignalAfterSyscall();

	void submitJoin(KernelSharedPtr<EventHub> event_hub, SubmitInfo submit_info);

	const uint64_t globalThreadId;
	
	uint32_t flags;

private:
	struct PendingSignal {
		PendingSignal(void *entry);

		void *entry;
	};

	struct JoinRequest : public BaseRequest {
		JoinRequest(KernelSharedPtr<EventHub> event_hub, SubmitInfo submit_info);
	};

	KernelSharedPtr<ThreadGroup> p_threadGroup;
	KernelSharedPtr<Universe> p_universe;
	KernelSharedPtr<AddressSpace> p_addressSpace;
	KernelSharedPtr<RdFolder> p_directory;

	KernelSharedPtr<Thread> p_nextInQueue;
	KernelUnsafePtr<Thread> p_previousInQueue;

	frigg::LinkedList<PendingSignal, KernelAlloc> p_pendingSignals;
	frigg::LinkedList<JoinRequest, KernelAlloc> p_joined;
};

struct ThreadGroup {
	static void addThreadToGroup(KernelSharedPtr<ThreadGroup> group,
			KernelWeakPtr<Thread> thread);

	ThreadGroup();

private:
	frigg::Vector<KernelWeakPtr<Thread>, KernelAlloc> p_members;
};

class ThreadQueue {
public:
	ThreadQueue();

	bool empty();

	void addBack(KernelSharedPtr<Thread> thread);
	
	KernelSharedPtr<Thread> removeFront();
	KernelSharedPtr<Thread> remove(KernelUnsafePtr<Thread> thread);

private:
	KernelSharedPtr<Thread> p_front;
	KernelUnsafePtr<Thread> p_back;
};

struct Signal {
	Signal(void *entry);

	void *entry;
};

} // namespace thor

