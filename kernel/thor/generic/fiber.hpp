#ifndef THOR_GENERIC_FIBER_HPP
#define THOR_GENERIC_FIBER_HPP

#include <frigg/callback.hpp>
#include <frg/container_of.hpp>
#include "../arch/x86/cpu.hpp"
#include "core.hpp"
#include "schedule.hpp"

namespace thor {

struct KernelFiber;
struct FiberBlocker;

KernelFiber *thisFiber();

struct KernelFiber : ScheduleEntity {
private:
	struct AssociatedWorkQueue : WorkQueue {
		void wakeup() override;
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
		return &_associatedWorkQueue;
	}

private:
	frigg::TicketLock _mutex;
	bool _blocked;

	AssociatedWorkQueue _associatedWorkQueue;
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

#endif // THOR_GENERIC_FIBER_HPP
