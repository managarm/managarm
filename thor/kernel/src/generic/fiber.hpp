#ifndef THOR_GENERIC_FIBER_HPP
#define THOR_GENERIC_FIBER_HPP

#include <frigg/callback.hpp>
#include <frg/container_of.hpp>
#include "../arch/x86/cpu.hpp"
#include "schedule.hpp"

namespace thor {

struct KernelFiber;

KernelFiber *thisFiber();

struct KernelFiber : ScheduleEntity {
	template<typename Node, typename Function, typename... Args>
	static void await(Function &&f, Args &&... args) {
		struct Awaiter {
			bool predicate() {
				return !complete.load(std::memory_order_relaxed);
			}

			Awaiter()
			: fiber{thisFiber()}, complete{false} { }

			Node node;
			KernelFiber *fiber;
			std::atomic<bool> complete;
		};

		Awaiter awaiter;
		f(frigg::forward<Args>(args)..., &awaiter.node, [] (Node *node) {
			auto awaiter = frg::container_of(node, &Awaiter::node);
			awaiter->complete.store(true, std::memory_order_relaxed);
			awaiter->fiber->unblock();
		});
		blockCurrent(CALLBACK_MEMBER(&awaiter, &Awaiter::predicate));
	}

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

	void unblock();

private:
	bool _blocked;
	FiberContext _context;
	Executor _executor;
};

} // namespace thor

#endif // THOR_GENERIC_FIBER_HPP
