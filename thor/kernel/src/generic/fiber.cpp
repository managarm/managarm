
#include "kernel.hpp"
#include "fiber.hpp"

namespace thor {

void KernelFiber::blockCurrent(frigg::CallbackPtr<bool()> predicate) {
	auto this_fiber = thisFiber();
	if(!predicate())
		return;
	
	assert(intsAreEnabled());
	disableInts();
	
	this_fiber->_blocked = true;
	getCpuData()->activeFiber = nullptr;
	Scheduler::suspend(this_fiber);

	if(forkExecutor(&this_fiber->_executor)) {
		runDetached([] {
			localScheduler()->reschedule();
		});
	}
	
	enableInts();
}

void KernelFiber::exitCurrent() {
	frigg::infoLogger() << "thor: Fix exiting fibers" << frigg::endLog;

	struct Predicate {
		bool always() {
			return true;
		}
	} p;

	KernelFiber::blockCurrent(CALLBACK_MEMBER(&p, &Predicate::always));

//	frigg::panicLogger() << "Fiber exited" << frigg::endLog;
}

void KernelFiber::run(UniqueKernelStack stack, void (*function)(void *), void *argument) {
	AbiParameters params;
	params.ip = (uintptr_t)function;
	params.argument = (uintptr_t)argument;

	auto fiber = frigg::construct<KernelFiber>(*kernelAlloc, std::move(stack), params);
	Scheduler::associate(fiber, localScheduler());
	Scheduler::resume(fiber);
}

KernelFiber::KernelFiber(UniqueKernelStack stack, AbiParameters abi)
: _blocked{false}, _context{std::move(stack)}, _executor{&_context, abi} { }

void KernelFiber::invoke() {
	getCpuData()->activeFiber = this;
	restoreExecutor(&_executor);
}

void KernelFiber::unblock() {
	if(!_blocked)
		return;
	
	_blocked = false;
	Scheduler::resume(this);
}

KernelFiber *thisFiber() {
	return getCpuData()->activeFiber;
}

} // namespace thor

