#ifndef THOR_GENERIC_FIBER_HPP
#define THOR_GENERIC_FIBER_HPP

#include <frigg/callback.hpp>
#include "../arch/x86/cpu.hpp"
#include "schedule.hpp"

namespace thor {

struct KernelFiber : ScheduleEntity {
	static void blockCurrent(frigg::CallbackPtr<bool()> predicate);

	static void exitCurrent();

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

	static void run(UniqueKernelStack stack, void (*function)(void *), void *argument);

	explicit KernelFiber(UniqueKernelStack stack, AbiParameters abi);

	[[ noreturn ]] void invoke() override;

	void unblock();

private:
	bool _blocked;
	FiberContext _context;
	Executor _executor;
};

KernelFiber *thisFiber();

} // namespace thor

#endif // THOR_GENERIC_FIBER_HPP
