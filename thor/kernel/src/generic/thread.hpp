#ifndef THOR_GENERIC_THREAD_HPP
#define THOR_GENERIC_THREAD_HPP

#include <atomic>
#include "core.hpp"
#include "error.hpp"
#include "schedule.hpp"

namespace thor {

enum Interrupt {
	kIntrNull,
	kIntrStop,
	kIntrPanic,
	kIntrBreakpoint,
	kIntrPageFault,
	kIntrGeneralFault,
	kIntrSuperCall = 0x80000000
};

struct Thread;

frigg::UnsafePtr<Thread> getCurrentThread();

struct Thread : frigg::SharedCounter, ScheduleEntity {
private:
	struct ObserveBase : Tasklet {
		void run() override;

		Error error;
		Interrupt interrupt;

		virtual void trigger(Error error, Interrupt interrupt) = 0;

		frg::default_list_hook<ObserveBase> hook;
	};

	template<typename F>
	struct Observe : ObserveBase {
		Observe(F functor)
		: _functor(frigg::move(functor)) { }

		void trigger(Error error, Interrupt interrupt) override {
			_functor(error, interrupt);
		}

	private:
		F _functor;
	};

public:
	static frigg::SharedPtr<Thread> create(frigg::SharedPtr<Universe> universe,
			frigg::SharedPtr<AddressSpace> address_space,
			AbiParameters abi) {
		auto thread = frigg::construct<Thread>(*kernelAlloc,
				frigg::move(universe), frigg::move(address_space), abi);
		return frigg::SharedPtr<Thread>{frigg::adoptShared, thread, 
				frigg::SharedControl{thread}};
	}

	template<typename P>
	static void blockCurrentWhile(P predicate) {
		// optimization: do not acquire the lock for the first test.
		if(!predicate())
			return;

		frigg::UnsafePtr<Thread> this_thread = getCurrentThread();
		while(true) {
			StatelessIrqLock irq_lock;
			auto guard = frigg::guard(&this_thread->_mutex);

			if(!predicate())
				return;

			_blockLocked(frigg::move(guard));
		}
	}

	// state transitions that apply to the current thread only.
	static void deferCurrent();
	static void deferCurrent(IrqImageAccessor image);
	static void interruptCurrent(Interrupt interrupt, FaultImageAccessor image);
	static void interruptCurrent(Interrupt interrupt, SyscallImageAccessor image);
	
	static void raiseSignals(SyscallImageAccessor image);

	// state transitions that apply to arbitrary threads.
	static void unblockOther(frigg::UnsafePtr<Thread> thread);
	static void interruptOther(frigg::UnsafePtr<Thread> thread);
	static void resumeOther(frigg::UnsafePtr<Thread> thread);

	// these signals let the thread change its RunState.
	// do not confuse them with POSIX signals!
	enum Signal {
		kSigNone,
		kSigStop
	};

	enum Flags : uint32_t {
		// disables preemption for this thread
		kFlagExclusive = 1,

		// traps kill the process instead of just halting it
		kFlagTrapsAreFatal = 2
	};

	Thread(frigg::SharedPtr<Universe> universe,
			frigg::SharedPtr<AddressSpace> address_space,
			AbiParameters abi);
	~Thread();

	const char *credentials() {
		return _credentials;
	}

	UserContext &getContext();
	frigg::UnsafePtr<Universe> getUniverse();
	frigg::UnsafePtr<AddressSpace> getAddressSpace();

	LaneHandle inferiorLane() {
		return _inferiorLane;
	}

	LaneHandle superiorLane() {
		return _superiorLane;
	}

	void signalStop();


	template<typename F>
	void submitObserve(F functor) {
		auto observe = frigg::construct<Observe<F>>(*kernelAlloc, frigg::move(functor));
		doSubmitObserve(observe);
	}

	// TODO: Do not expose these functions publically.
	void destruct() override;
	void cleanup() override;

	[[ noreturn ]] void invoke() override;
	
	void doSubmitObserve(ObserveBase *observe);

	// TODO: Tidy this up.
	frigg::UnsafePtr<Thread> self;

	uint32_t flags;

private:
	typedef frigg::TicketLock Mutex;

	enum RunState {
		kRunNone,

		// the thread is running on some processor.
		kRunActive,

		// the thread is in the schedule queue but not active on any processor.
		// it may be killed in this state.
		kRunSuspended,

		// like kRunSuspended but the thread must not be killed in this state.
		kRunDeferred,

		// the thread is waiting for progress inside the kernel.
		// it is not scheduled.
		kRunBlocked,

		// the thread was manually stopped from userspace.
		// it is not scheduled.
		kRunInterrupted,
	};

	static void _blockLocked(frigg::LockGuard<Mutex> lock);

	char _credentials[16];

	Mutex _mutex;

	RunState _runState;

	// number of ticks this thread has been running (i.e. in the active state)
	uint64_t _numTicks;

	// tick in which this thread transitioned to the active state
	uint64_t _activationTick;

	// This is set by signalStop() and polled by raiseSignals().
	Signal _pendingSignal;

	// Number of references that keep this thread running.
	// The thread is killed when this counter reaches zero.
	std::atomic<int> _runCount;

	UserContext _context;
public:
	// TODO: This should be private.
	Executor _executor;

private:
	frigg::SharedPtr<Universe> _universe;
	frigg::SharedPtr<AddressSpace> _addressSpace;

	LaneHandle _superiorLane;
	LaneHandle _inferiorLane;

	frg::intrusive_list<
		ObserveBase,
		frg::locate_member<
			ObserveBase,
			frg::default_list_hook<ObserveBase>,
			&ObserveBase::hook
		>
	> _observeQueue;
};

} // namespace thor

#endif // THOR_GENERIC_THREAD_HPP
