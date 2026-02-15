#pragma once

#include <atomic>

#include <frg/container_of.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/credentials.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/schedule.hpp>
#include <thor-internal/universe.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

// Bitwise flags that cause a thread to take some asynchronous action.
using Condition = uint32_t;

// Conditions are dequeued in descending order (i.e., high condition bits take priority).
// TODO: Integrate asynchronous WorkQueue runs into Condition.
// TODO: Integrate thread killing into Condition.
// TODO: Integrate CPU migration into Condition.
namespace condition {
// Request kIntrRequested to be raised.
inline constexpr Condition interrupt = Condition{1} << 0;
} // namespace condition

enum Interrupt {
	kIntrNull,
	kIntrDivByZero,
	kIntrRequested,
	kIntrPanic,
	kIntrBreakpoint,
	kIntrPageFault,
	kIntrGeneralFault,
	kIntrIllegalInstruction,
	kIntrSuperCall = 0x80000000
};

enum class PageFaultType {
	None,
	NotMapped,
	BadPermissions,
};

struct InterruptInfo {
	uintptr_t offendingAddress = 0;
	PageFaultType pageFaultType = PageFaultType::None;
};

struct AsyncBlockCurrentInterruptibleTag {};
struct AsyncBlockCurrentNormalTag {};

template <typename T>
concept AnyTag = (std::same_as<T, AsyncBlockCurrentNormalTag> || std::same_as<T, AsyncBlockCurrentInterruptibleTag>);

// Shift for fixed point numbers that represent the load level.
constexpr int loadShift = 10;

struct Thread;
struct LbControlBlock;

template <template<typename, typename> typename Ptr, typename T, typename H>
	requires (!std::is_same_v<H, void>)
Ptr<T, void> remove_tag_cast(const Ptr<T, H> &other) {
	other.ctr()->holder()->increment();
	auto ret = Ptr<T, void>{smarter::adopt_rc, other.get(), other.ctr()->holder()};
	return ret;
}

smarter::borrowed_ptr<Thread> getCurrentThread();

struct ActiveHandle { };

struct Thread final : smarter::crtp_counter<Thread, ActiveHandle>, ScheduleEntity, Credentials {
	// Silence Clang warning about hidden overloads.
	using smarter::crtp_counter<Thread, ActiveHandle>::dispose;

private:
	struct AssociatedWorkQueue final : WorkQueue {
		AssociatedWorkQueue(Thread *thread, Ipl wqIpl)
		: WorkQueue{&thread->_executorContext, wqIpl}, _thread{thread} { }

		void wakeup() override;

	private:
		Thread *_thread;
	};

public:
	static smarter::shared_ptr<Thread, ActiveHandle> create(smarter::shared_ptr<Universe> universe,
			smarter::shared_ptr<AddressSpace, BindableHandle> address_space,
			AbiParameters abi) {
		auto thread = smarter::allocate_shared<Thread>(*kernelAlloc,
				std::move(universe), std::move(address_space), abi);
		thread->_executorContext.exceptionalWq = &thread->_pagingWorkQueue;

		// The kernel owns one reference to the thread until the thread finishes execution.
		thread.ctr()->increment();

		auto ptr = thread.get();
		ptr->setup(smarter::adopt_rc, thread.ctr(), 1);
		thread.release();
		smarter::shared_ptr<Thread, ActiveHandle> sptr{smarter::adopt_rc, ptr, ptr};

		return sptr;
	}

	template <typename Sender>
	static auto asyncBlockCurrent(Sender s, WorkQueue *wq) {
		return asyncBlockCurrent([s = std::move(s)](async::cancellation_token) mutable -> Sender {
			return std::move(s);
		}, wq, AsyncBlockCurrentNormalTag{});
	}

	template <typename SenderFactory>
	requires(std::is_invocable_v<SenderFactory, async::cancellation_token>)
	static auto asyncBlockCurrentInterruptible(SenderFactory s, WorkQueue *wq) {
		return asyncBlockCurrent(std::move(s), wq, AsyncBlockCurrentInterruptibleTag{});
	}

	template <typename SenderFactory, AnyTag Tag>
	    requires(std::is_invocable_v<SenderFactory, async::cancellation_token>
	             && std::is_same_v<
	                 typename std::invoke_result_t<SenderFactory, async::cancellation_token>::value_type,
	                 void>)
	static void asyncBlockCurrent(SenderFactory s, WorkQueue *wq, Tag tag) {
		(void)tag;
		auto thisThread = getCurrentThread();

		async::cancellation_event ce;
		auto sv = std::move(s(async::cancellation_token{ce}));

		struct BlockingState {
			// We need a shared_ptr since the thread might continue (and thus could be killed)
			// immediately after we set the done flag.
			smarter::shared_ptr<Thread> thread;
			WorkQueue *wq;
			// Acquire-release semantics to publish the result of the async operation.
			std::atomic<bool> done{false};
		} bls {.thread = thisThread.lock(), .wq = wq};

		struct Env {
			WorkQueue *get_work_queue() {
				return blsp->wq;
			}

			BlockingState *blsp;
		};

		struct Receiver {
			void set_value_inline() {
				// Do nothing (there is no value to store).
			}

			void set_value_noinline() {
				// The blsp pointer may become invalid as soon as we set blsp->done.
				auto thread = std::move(blsp->thread);
				assert(!blsp->done.load(std::memory_order_relaxed));
				blsp->done.store(true, std::memory_order_release);
				Thread::unblockOther(thread);
			}

			auto get_env() {
				return Env{.blsp = blsp};
			}

			BlockingState *blsp;
		};

		bool interruptible = std::is_same_v<Tag, AsyncBlockCurrentInterruptibleTag>;

		auto operation = async::execution::connect(std::move(sv), Receiver{&bls});
		if(async::execution::start_inline(operation))
			return;
		while(true) {
			if(bls.done.load(std::memory_order_acquire))
				break;
			if(runWqs()) {
				// Re-check the done flag since nested blocking (triggered by the WQ)
				// might have consumed the unblock latch.
				continue;
			}
			if (!Thread::blockCurrent(interruptible)) {
				ce.cancel();
				interruptible = false;
				continue;
			}
		}

		return;
	}

	template <typename SenderFactory, AnyTag Tag>
	    requires(
	        (std::is_invocable_v<SenderFactory, async::cancellation_token>
	         && !std::is_same_v<
	             typename std::invoke_result_t<SenderFactory, async::cancellation_token>::value_type,
	             void>)
	    )
	static auto asyncBlockCurrent(SenderFactory s, WorkQueue *wq, Tag tag) {
		(void)tag;
		auto thisThread = getCurrentThread();

		async::cancellation_event ce;
		auto sv = std::move(s(async::cancellation_token{ce}));

		using ValueType = typename decltype(sv)::value_type;

		struct BlockingState {
			// We need a shared_ptr since the thread might continue (and thus could be killed)
			// immediately after we set the done flag.
			smarter::shared_ptr<Thread> thread;
			WorkQueue *wq;
			// Acquire-release semantics to publish the result of the async operation.
			std::atomic<bool> done{false};
			frg::optional<ValueType> value{};
		} bls{.thread = thisThread.lock(), .wq = wq};

		struct Env {
			WorkQueue *get_work_queue() {
				return blsp->wq;
			}

			BlockingState *blsp;
		};

		struct Receiver {
			void set_value_inline(ValueType value) { blsp->value.emplace(std::move(value)); }

			void set_value_noinline(ValueType value) {
				// The blsp pointer may become invalid as soon as we set blsp->done.
				auto thread = std::move(blsp->thread);
				assert(!blsp->done.load(std::memory_order_relaxed));
				blsp->value.emplace(std::move(value));
				blsp->done.store(true, std::memory_order_release);
				Thread::unblockOther(thread);
			}

			auto get_env() {
				return Env{.blsp = blsp};
			}

			BlockingState *blsp;
		};

		bool interruptible = std::is_same_v<Tag, AsyncBlockCurrentInterruptibleTag>;

		auto operation = async::execution::connect(std::move(sv), Receiver{&bls});
		if(async::execution::start_inline(operation))
			return std::move(*bls.value);
		while(true) {
			if(bls.done.load(std::memory_order_acquire))
				break;
			if(runWqs()) {
				// Re-check the done flag since nested blocking (triggered by the WQ)
				// might have consumed the unblock latch.
				continue;
			}
			if (!Thread::blockCurrent(interruptible)) {
				ce.cancel();
				interruptible = false;
				continue;
			}
		}
		return std::move(*bls.value);
	}

	// Run the current thread's WQs. Returns true if there was any work to do.
	static bool runWqs() {
		auto ipl = currentIpl();
		auto thisThread = getCurrentThread();
		if (ipl <= ipl::exceptional) {
			if (thisThread->_pagingWorkQueue.check()) {
				thisThread->_pagingWorkQueue.run();
				return true;
			}
		}
		if (ipl == ipl::passive) {
			if (thisThread->_mainWorkQueue.check()) {
				thisThread->_mainWorkQueue.run();
				return true;
			}
		}
		return false;
	}

	// Run the current thread's WQ until they are empty.
	static void drainWqs() {
		while (runWqs())
			;
	}

	// State transitions that apply to the current thread only.

	// returns false if the block was interrupted
	static bool blockCurrent(bool interruptible = false);
	static void migrateCurrent();
	static void deferCurrent();
	static void deferCurrent(IrqImageAccessor image);
	static void suspendCurrent(IrqImageAccessor image);
	static void interruptCurrent(Interrupt interrupt, FaultImageAccessor image, InterruptInfo info);
	static void interruptCurrent(Interrupt interrupt, SyscallImageAccessor image, InterruptInfo info);

	static void handleConditions(SyscallImageAccessor image);
	static void raiseSignals(SyscallImageAccessor image);

	// State transitions that apply to arbitrary threads.
	// TODO: interruptOther() needs an Interrupt argument.
	static void unblockOther(smarter::borrowed_ptr<Thread> thread);
	static void killOther(smarter::borrowed_ptr<Thread> thread);
	static void interruptOther(smarter::borrowed_ptr<Thread> thread);
	static Error resumeOther(smarter::borrowed_ptr<Thread> thread);

	enum Flags : uint32_t {
		kFlagServer = 1
	};

	Thread(smarter::shared_ptr<Universe> universe,
			smarter::shared_ptr<AddressSpace, BindableHandle> address_space,
			AbiParameters abi);
	~Thread();

	smarter::borrowed_ptr<WorkQueue> mainWorkQueue() {
		return {&_mainWorkQueue, self.ctr()};
	}
	smarter::borrowed_ptr<WorkQueue> pagingWorkQueue() {
		return {&_pagingWorkQueue, self.ctr()};
	}

	UserContext &getContext();
	smarter::borrowed_ptr<Universe> getUniverse();
	smarter::borrowed_ptr<AddressSpace, BindableHandle> getAddressSpace();

	// ----------------------------------------------------------------------------------
	// observe() and its boilerplate.
	// ----------------------------------------------------------------------------------
private:
	struct ObserveNode {
		async::any_receiver<frg::tuple<Error, uint64_t, Interrupt>> receiver;
		frg::default_list_hook<ObserveNode> hook = {};
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
	void dispose(ActiveHandle); // Called when shared_ptr refcount reaches zero.

	[[ noreturn ]] void invoke() override;

	void handlePreemption(IrqImageAccessor image) override;
	// Non-virtual since syscalls/faults know that they are called from a thread.
	void handlePreemption(FaultImageAccessor image);
	void handlePreemption(SyscallImageAccessor image);

	InterruptInfo interruptInfo;

private:
	template<typename ImageAccessor>
	void doHandlePreemption(bool inManipulableDomain, ImageAccessor image);

	void _updateRunTime();
	void _uninvoke();
	void _kill();

public:
	// TODO: Tidy this up.
	smarter::borrowed_ptr<Thread> self;

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

		// thread is waiting for progress inside the kernel.
		// it is not scheduled, but it can be interrupted.
		kRunInterruptableBlocked,

		// the thread was manually stopped from userspace.
		// it is not scheduled.
		kRunInterrupted,

		// Thread exited or was killed.
		kRunTerminated
	};

	AssociatedWorkQueue _mainWorkQueue;
	AssociatedWorkQueue _pagingWorkQueue;

	Mutex _mutex;

	RunState _runState;

	// If this flag is set, blockCurrent() returns immediately.
	// In blockCurrent(), the flag is checked within _mutex.
	// On 0-1 transitions, we take _mutex and try to unblock the thread.
	// Since _mutex enforces a total order, this guarantees correctness
	// (i.e., that we never block when we should not).
	std::atomic<bool> _unblockLatch{false};

	Interrupt _lastInterrupt;
	uint64_t _stateSeq;

	// This is set by interruptOther() and polled by raiseSignals().
	bool _pendingKill;
	// Conditions are raised while holding the thread mutex.
	// In particular, functions like blockCurrent() can be sure that no conditions
	// become pending while they are modifying the thread state.
	// Conditions can be read and cleared without holding the mutex.
	// Conditions can only be cleared from the thread itself.
	std::atomic<Condition> _pendingConditions{0};

	// Number of references that keep this thread running.
	// The thread is killed when this counter reaches zero.
	std::atomic<int> _runCount;

	UserContext _userContext;
	ExecutorContext _executorContext;
public:
	// TODO: This should be private.
	Executor _executor;

	// Timestamp at which _updateRunTime() was last called.
	uint64_t _lastRunTimeUpdate{0};
	// Contributions to the load factor due to time during which the thread was (not) runnable.
	// The thread is runnable if it is either running or waiting in a scheduler queue
	// (i.e., not blocked).
	uint64_t _loadRunnable{0};
	uint64_t _loadNotRunnable{0};
	// Load level of the thread.
	std::atomic<uint64_t> _loadLevel{0};

	// Update the load factor.
	void updateLoad();
	// Called periodically by load balancing code.
	void decayLoad(uint64_t decayFactor, int decayScale);

	// Return the load factor.
	uint64_t loadLevel() {
		return _loadLevel.load(std::memory_order_relaxed);
	}

	LbControlBlock *_lbCb{nullptr};

private:
	smarter::shared_ptr<Universe> _universe;
	smarter::shared_ptr<AddressSpace, BindableHandle> _addressSpace;

	using ObserveQueue = frg::intrusive_list<
		ObserveNode,
		frg::locate_member<
			ObserveNode,
			frg::default_list_hook<ObserveNode>,
			&ObserveNode::hook
		>
	>;

	ObserveQueue _observeQueue;
};

} // namespace thor
