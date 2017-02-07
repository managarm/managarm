
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

void KernelFiber::run(void (*function)()) {
	AbiParameters params;
	params.ip = (uintptr_t)function;

	auto fiber = frigg::construct<KernelFiber>(*kernelAlloc, params);
	globalScheduler().attach(fiber);
	globalScheduler().resume(fiber);
}

KernelFiber::KernelFiber(AbiParameters abi)
: _blocked{false}, _executor{&_context, abi} { }

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

