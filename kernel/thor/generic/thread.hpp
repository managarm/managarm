#ifndef THOR_GENERIC_THREAD_HPP
#define THOR_GENERIC_THREAD_HPP

#include <string.h>
#include <atomic>

#include <frg/container_of.hpp>
#include "core.hpp"
#include "error.hpp"
#include "schedule.hpp"
#include "work-queue.hpp"

namespace thor {

enum Interrupt {
	kIntrNull,
	kIntrRequested,
	kIntrPanic,
	kIntrBreakpoint,
	kIntrPageFault,
	kIntrGeneralFault,
	kIntrIllegalInstruction,
	kIntrSuperCall = 0x80000000
};

struct Thread;
struct ThreadBlocker;

struct ThreadBlocker {
	friend struct Thread;

	void setup();

private:
	Thread *_thread;
	bool _done;
};

frigg::UnsafePtr<Thread> getCurrentThread();

struct Thread final : frigg::SharedCounter, ScheduleEntity {
private:
	struct ObserveBase {
		Error error;
		uint64_t sequence;
		Interrupt interrupt;

		Worklet *triggered;
		frg::default_list_hook<ObserveBase> hook;
	};

	template<typename F>
	struct Observe : ObserveBase {
		static void trigger(Worklet *base) {
			auto self = frg::container_of(base, &Observe::_worklet);
			self->_functor(self->error, self->sequence, self->interrupt);
			frigg::destruct(*kernelAlloc, self);
		}

		Observe(F functor)
		: _functor(frigg::move(functor)) {
			_worklet.setup(&Observe::trigger);
			triggered = &_worklet;
		}

	private:
		Worklet _worklet;
		F _functor;
	};

	struct AssociatedWorkQueue : WorkQueue {
		AssociatedWorkQueue(Thread *thread)
		: _thread{thread} { }

		void wakeup() override;
	
	private:
		Thread *_thread;
	};

public:
	static frigg::SharedPtr<Thread> create(frigg::SharedPtr<Universe> universe,
			smarter::shared_ptr<AddressSpace, BindableHandle> address_space,
			AbiParameters abi) {
		auto thread = frigg::construct<Thread>(*kernelAlloc,
				frigg::move(universe), frigg::move(address_space), abi);
		frigg::SharedPtr<Thread> sptr{frigg::adoptShared, thread,
				frigg::SharedControl{thread}};
		thread->_mainWorkQueue.selfPtr = frigg::SharedPtr<WorkQueue>(sptr,
				&thread->_mainWorkQueue);
		thread->_pagingWorkQueue.selfPtr = frigg::SharedPtr<WorkQueue>(sptr,
				&thread->_pagingWorkQueue);
		return sptr;
	}

	template<typename Sender>
	static void asyncBlockCurrent(Sender s) {
		struct Closure {
			ThreadBlocker blocker;
		} closure;

		struct Receiver {
			void set_value() {
				Thread::unblockOther(&closure->blocker);
			}

			Closure *closure;
		};

		closure.blocker.setup();
		auto operation = async::execution::connect(std::move(s), Receiver{&closure});
		async::execution::start(operation);
		Thread::blockCurrent(&closure.blocker);
	}

	// State transitions that apply to the current thread only.
	static void blockCurrent(ThreadBlocker *blocker);
	static void migrateCurrent();
	static void deferCurrent();
	static void deferCurrent(IrqImageAccessor image);
	static void suspendCurrent(IrqImageAccessor image);
	static void interruptCurrent(Interrupt interrupt, FaultImageAccessor image);
	static void interruptCurrent(Interrupt interrupt, SyscallImageAccessor image);
	
	static void raiseSignals(SyscallImageAccessor image);

	// State transitions that apply to arbitrary threads.
	// TODO: interruptOther() needs an Interrupt argument.
	static void unblockOther(ThreadBlocker *blocker);
	static void killOther(frigg::UnsafePtr<Thread> thread);
	static void interruptOther(frigg::UnsafePtr<Thread> thread);
	static Error resumeOther(frigg::UnsafePtr<Thread> thread);

	// These signals let the thread change its RunState.
	// Do not confuse them with POSIX signals!
	// TODO: Interrupt signals should be queued.
	enum Signal {
		kSigNone,
		kSigInterrupt
	};

	enum Flags : uint32_t {
		kFlagServer = 1
	};

	Thread(frigg::SharedPtr<Universe> universe,
			smarter::shared_ptr<AddressSpace, BindableHandle> address_space,
			AbiParameters abi);
	~Thread();

	const char *credentials() {
		return _credentials;
	}
	
	WorkQueue *mainWorkQueue() {
		return &_mainWorkQueue;
	}
	WorkQueue *pagingWorkQueue() {
		return &_pagingWorkQueue;
	}

	UserContext &getContext();
	frigg::UnsafePtr<Universe> getUniverse();
	smarter::borrowed_ptr<AddressSpace, BindableHandle> getAddressSpace();

	LaneHandle inferiorLane() {
		return _inferiorLane;
	}

	LaneHandle superiorLane() {
		return _superiorLane;
	}

	template<typename F>
	void submitObserve(uint64_t in_seq, F functor) {
		auto observe = frigg::construct<Observe<F>>(*kernelAlloc, frigg::move(functor));
		doSubmitObserve(in_seq, observe);
	}

	// TODO: Do not expose these functions publically.
	void destruct() override; // Called when shared_ptr refcount reaches zero.
	void cleanup() override; // Called when weak_ptr refcount reaches zero.

	[[ noreturn ]] void invoke() override;

private:
	void _uninvoke();
	void _kill();

public:
	void doSubmitObserve(uint64_t in_seq, ObserveBase *observe);
	void setAffinityMask(frg::vector<uint8_t, KernelAlloc> &&mask) {
		auto lock = frigg::guard(&_mutex);
		_affinityMask = std::move(mask);
	}

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

		// Thread exited or was killed.
		kRunTerminated
	};

	static void _blockLocked(frigg::LockGuard<Mutex> lock);

	char _credentials[16];

	AssociatedWorkQueue _mainWorkQueue;
	AssociatedWorkQueue _pagingWorkQueue;

	Mutex _mutex;

	RunState _runState;
	Interrupt _lastInterrupt;
	uint64_t _stateSeq;

	// number of ticks this thread has been running (i.e. in the active state)
	uint64_t _numTicks;

	// tick in which this thread transitioned to the active state
	uint64_t _activationTick;

	// This is set by interruptOther() and polled by raiseSignals().
	bool _pendingKill;
	Signal _pendingSignal;

	// Number of references that keep this thread running.
	// The thread is killed when this counter reaches zero.
	std::atomic<int> _runCount;

	UserContext _userContext;
	ExecutorContext _executorContext;
public:
	// TODO: This should be private.
	Executor _executor;

private:
	frigg::SharedPtr<Universe> _universe;
	smarter::shared_ptr<AddressSpace, BindableHandle> _addressSpace;

	LaneHandle _superiorLane;
	LaneHandle _inferiorLane;
	
	using ObserveQueue = frg::intrusive_list<
		ObserveBase,
		frg::locate_member<
			ObserveBase,
			frg::default_list_hook<ObserveBase>,
			&ObserveBase::hook
		>
	>;

	ObserveQueue _observeQueue;
	frg::vector<uint8_t, KernelAlloc> _affinityMask;
};

} // namespace thor

#endif // THOR_GENERIC_THREAD_HPP
