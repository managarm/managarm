
namespace thor {

enum Fault {
	kFaultNone,
	kFaultBreakpoint
};

class Thread : public PlatformExecutor {
friend class ThreadRunControl;
public:
	// these signals let the thread change its RunState.
	// do not confuse them with POSIX signals!
	enum Signal {
		kSigNone,
		kSigKill
	};

	enum Flags : uint32_t {
		// disables preemption for this thread
		kFlagExclusive = 1,

		// thread is not enqueued in the scheduling queue
		// e.g. this is set for the per-cpu idle threads
		kFlagNotScheduled = 2,

		// traps kill the process instead of just halting it
		kFlagTrapsAreFatal = 4
	};

	Thread(KernelSharedPtr<Universe> universe,
			KernelSharedPtr<AddressSpace> address_space,
			KernelSharedPtr<RdFolder> directory);
	~Thread();

	KernelUnsafePtr<Universe> getUniverse();
	KernelUnsafePtr<AddressSpace> getAddressSpace();
	KernelUnsafePtr<RdFolder> getDirectory();

	void signalKill();
	Signal pendingSignal();

	void transitionToFault();
	void resume();

	void submitObserve(KernelSharedPtr<AsyncObserve> observe);

	uint32_t flags;

private:
	enum RunState {
		kRunNone,
		kRunActive,
		kRunSaved,
		kRunFaulted,
		kRunInterrupted
	};

	RunState _runState;

	// this is set by signalKill() and queried each time the kernel
	// is ready to process the kill request, e.g. after a syscall finishes.
	Signal _pendingSignal;

	// number of references that keep this thread running.
	// the thread is killed when this counter reaches zero.
	int _runCount;

	KernelSharedPtr<Universe> p_universe;
	KernelSharedPtr<AddressSpace> p_addressSpace;
	KernelSharedPtr<RdFolder> p_directory;

	frigg::IntrusiveSharedLinkedList<
		AsyncObserve,
		&AsyncObserve::processQueueItem
	> _observeQueue;
};

} // namespace thor

