#pragma once

#include <frg/container_of.hpp>
#include <frigg/callback.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/core.hpp>
#include <thor-internal/schedule.hpp>

namespace thor {

struct KernelFiber;
struct FiberBlocker;

KernelFiber *thisFiber();

struct KernelFiber : ScheduleEntity {
private:
	struct AssociatedWorkQueue final : WorkQueue {
		AssociatedWorkQueue(KernelFiber *fiber)
		: fiber_{fiber} { }

		void wakeup() override;

	private:
		KernelFiber *fiber_;
	};

public:
	static void blockCurrent(FiberBlocker *blocker);
	static void exitCurrent();

	static void unblockOther(FiberBlocker *blocker);

	template<typename F>
	static void run(F functor) {
		auto frame = [] (void *argument) {
			auto object = reinterpret_cast<F *>(argument);
			(*object)();
			exitCurrent();
		};
		auto stack = UniqueKernelStack::make();
		auto target = stack.embed<F>(functor);
		run(std::move(stack), frame, target);
	}

	template<typename F>
	static KernelFiber *post(F functor) {
		auto frame = [] (void *argument) {
			auto object = reinterpret_cast<F *>(argument);
			(*object)();
			exitCurrent();
		};
		auto stack = UniqueKernelStack::make();
		auto target = stack.embed<F>(functor);
		return post(std::move(stack), frame, target);
	}

	static void run(UniqueKernelStack stack, void (*function)(void *), void *argument);
	static KernelFiber *post(UniqueKernelStack stack, void (*function)(void *), void *argument);

	explicit KernelFiber(UniqueKernelStack stack, AbiParameters abi);

	[[ noreturn ]] void invoke() override;

	WorkQueue *associatedWorkQueue() {
		return _associatedWorkQueue.get();
	}

private:
	frigg::TicketLock _mutex;
	bool _blocked;

	frigg::SharedPtr<AssociatedWorkQueue> _associatedWorkQueue;
	FiberContext _fiberContext;
	ExecutorContext _executorContext;
	Executor _executor;
};

struct FiberBlocker {
	friend struct KernelFiber;

	void setup();

private:
	KernelFiber *_fiber;
	bool _done;
};

} // namespace thor
