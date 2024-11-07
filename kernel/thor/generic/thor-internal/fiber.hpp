#pragma once

#include <frg/container_of.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/schedule.hpp>
#include <initgraph.hpp>

namespace thor {

// Once this stage is reached, the kernel can launch fibers.
// (Even though they do not necessarily start yet.)
initgraph::Stage *getFibersAvailableStage();

struct KernelFiber;

KernelFiber *thisFiber();

struct FiberBlocker {
	friend struct KernelFiber;

	void setup();

private:
	KernelFiber *_fiber;
	bool _done;
};

struct KernelFiber final : ScheduleEntity {
private:
	struct AssociatedWorkQueue final : WorkQueue {
		AssociatedWorkQueue(KernelFiber *fiber)
		: WorkQueue{&fiber->_executorContext}, fiber_{fiber} { }

		void wakeup() override;

	private:
		KernelFiber *fiber_;
	};

public:
	static void blockCurrent(FiberBlocker *blocker);

	template<typename Sender>
	requires std::is_same_v<typename Sender::value_type, void>
	static void asyncBlockCurrent(Sender s) {
		struct Closure {
			FiberBlocker blocker;
		} closure;

		struct Receiver {
			void set_value_inline() {
				// Do nothing (there is no value to store).
			}

			void set_value_noinline() {
				KernelFiber::unblockOther(&closure->blocker);
			}

			Closure *closure;
		};

		closure.blocker.setup();
		auto operation = async::execution::connect(std::move(s), Receiver{&closure});
		if(async::execution::start_inline(operation))
			return;
		KernelFiber::blockCurrent(&closure.blocker);
	}

	template<typename Sender>
	requires (!std::is_same_v<typename Sender::value_type, void>)
	static typename Sender::value_type asyncBlockCurrent(Sender s) {
		struct Closure {
			frg::optional<typename Sender::value_type> value;
			FiberBlocker blocker;
		} closure;

		struct Receiver {
			void set_value_inline(typename Sender::value_type value) {
				closure->value.emplace(std::move(value));
			}

			void set_value_noinline(typename Sender::value_type value) {
				closure->value.emplace(std::move(value));
				KernelFiber::unblockOther(&closure->blocker);
			}

			Closure *closure;
		};

		closure.blocker.setup();
		auto operation = async::execution::connect(std::move(s), Receiver{&closure});
		if(async::execution::start_inline(operation))
			return std::move(*closure.value);
		KernelFiber::blockCurrent(&closure.blocker);
		return std::move(*closure.value);
	}

	static void exitCurrent();

	static void unblockOther(FiberBlocker *blocker);

	template<typename F>
	static void run(F functor, Scheduler *scheduler = localScheduler()) {
		auto frame = [] (void *argument) {
			auto object = reinterpret_cast<F *>(argument);
			(*object)();
			exitCurrent();
		};
		auto stack = UniqueKernelStack::make();
		auto target = stack.embed<F>(functor);
		run(std::move(stack), frame, target, scheduler);
	}

	template<typename F>
	static KernelFiber *post(F functor, Scheduler *scheduler = localScheduler()) {
		auto frame = [] (void *argument) {
			auto object = reinterpret_cast<F *>(argument);
			(*object)();
			exitCurrent();
		};
		auto stack = UniqueKernelStack::make();
		auto target = stack.embed<F>(functor);
		return post(std::move(stack), frame, target, scheduler);
	}

	static void run(UniqueKernelStack stack, void (*function)(void *), void *argument, Scheduler* = localScheduler());
	static KernelFiber *post(UniqueKernelStack stack, void (*function)(void *), void *argument, Scheduler* = localScheduler());

	explicit KernelFiber(UniqueKernelStack stack, AbiParameters abi);

	[[ noreturn ]] void invoke() override;

	void handlePreemption(IrqImageAccessor) override;

	WorkQueue *associatedWorkQueue() {
		return _associatedWorkQueue.get();
	}

private:
	frg::ticket_spinlock _mutex;
	bool _blocked;

	smarter::shared_ptr<AssociatedWorkQueue> _associatedWorkQueue;
	FiberContext _fiberContext;
	ExecutorContext _executorContext;
	Executor _executor;
};

} // namespace thor
