#pragma once

#include <string.h>
#include <atomic>

#include <frg/container_of.hpp>
#include <thor-internal/core.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/schedule.hpp>
#include <thor-internal/work-queue.hpp>

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
		auto thread = frg::construct<Thread>(*kernelAlloc,
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
	requires std::is_same_v<typename Sender::value_type, void>
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

	template<typename Sender>
	requires (!std::is_same_v<typename Sender::value_type, void>)
	static typename Sender::value_type asyncBlockCurrent(Sender s) {
		struct Closure {
			frg::optional<typename Sender::value_type> value;
			ThreadBlocker blocker;
		} closure;

		struct Receiver {
			void set_value(typename Sender::value_type value) {
				closure->value.emplace(std::move(value));
				Thread::unblockOther(&closure->blocker);
			}

			Closure *closure;
		};

		closure.blocker.setup();
		auto operation = async::execution::connect(std::move(s), Receiver{&closure});
		async::execution::start(operation);
		Thread::blockCurrent(&closure.blocker);
		return std::move(*closure.value);
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

	// ----------------------------------------------------------------------------------
	// observe() and its boilerplate.
	// ----------------------------------------------------------------------------------
private:
	struct ObserveNode {
		async::any_receiver<frg::tuple<Error, uint64_t, Interrupt>> receiver;
		frg::default_list_hook<ObserveNode> hook;
	};

	void observe_(uint64_t inSeq, ObserveNode *node);

public:
	template<typename Receiver>
	struct [[nodiscard]] ObserveOperation {
		ObserveOperation(Thread *self, uint64_t inSeq, Receiver receiver)
		: self_{self}, inSeq_{inSeq}, node_{.receiver = std::move(receiver)} { }

		void start() {
			self_->observe_(inSeq_, &node_);
		}

	private:
		Thread *self_;
		uint64_t inSeq_;
		ObserveNode node_;
	};

	struct [[nodiscard]] ObserveSender {
		using value_type = frg::tuple<Error, uint64_t, Interrupt>;

		async::sender_awaiter<ObserveSender, frg::tuple<Error, uint64_t, Interrupt>>
		operator co_await() {
			return {std::move(*this)};
		}

		template<typename Receiver>
		ObserveOperation<Receiver> connect(Receiver receiver) {
			return {self, inSeq, std::move(receiver)};
		}

		Thread *self;
		uint64_t inSeq;
	};

	ObserveSender observe(uint64_t inSeq) {
		return {this, inSeq};
	}

	// ----------------------------------------------------------------------------------

	// TODO: Do not expose these functions publically.
	void destruct() override; // Called when shared_ptr refcount reaches zero.
	void cleanup() override; // Called when weak_ptr refcount reaches zero.

	[[ noreturn ]] void invoke() override;

private:
	void _uninvoke();
	void _kill();

public:
	void setAffinityMask(frg::vector<uint8_t, KernelAlloc> &&mask) {
		auto lock = frg::guard(&_mutex);
		_affinityMask = std::move(mask);
	}

	// TODO: Tidy this up.
	frigg::UnsafePtr<Thread> self;

	uint32_t flags;

private:
	typedef frg::ticket_spinlock Mutex;

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
		ObserveNode,
		frg::locate_member<
			ObserveNode,
			frg::default_list_hook<ObserveNode>,
			&ObserveNode::hook
		>
	>;

	ObserveQueue _observeQueue;
	frg::vector<uint8_t, KernelAlloc> _affinityMask;
};

} // namespace thor
