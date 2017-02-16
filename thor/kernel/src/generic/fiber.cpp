
#include "kernel.hpp"
#include "fiber.hpp"

namespace thor {

void KernelFiber::blockCurrent(frigg::CallbackPtr<bool()> predicate) {
	auto this_fiber = thisFiber();
	if(!predicate())
		return;
	
	this_fiber->_blocked = true;
	getCpuData()->activeFiber = nullptr;
	globalScheduler().suspend(this_fiber);

	if(forkExecutor(&this_fiber->_executor)) {
		runDetached([] {
			globalScheduler().reschedule();
		});
	}
}

void KernelFiber::exitCurrent() {
	frigg::panicLogger() << "Fiber exited" << frigg::endLog;
}

void KernelFiber::run(UniqueKernelStack stack, void (*function)(void *), void *argument) {
	AbiParameters params;
	params.ip = (uintptr_t)function;
	params.argument = (uintptr_t)argument;

	auto fiber = frigg::construct<KernelFiber>(*kernelAlloc, std::move(stack), params);
	globalScheduler().attach(fiber);
	globalScheduler().resume(fiber);
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
	globalScheduler().resume(this);
}

KernelFiber *thisFiber() {
	return getCpuData()->activeFiber;
}

} // namespace thor

