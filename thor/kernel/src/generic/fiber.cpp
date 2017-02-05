
#include "kernel.hpp"
#include "fiber.hpp"

namespace thor {

void KernelFiber::run(void (*function)()) {
	AbiParameters params;
	params.ip = (uintptr_t)function;

	auto fiber = frigg::construct<KernelFiber>(*kernelAlloc, params);
	globalScheduler().attach(fiber);
	globalScheduler().resume(fiber);
}

void KernelFiber::invoke() {
	restoreExecutor(&_executor);
}

KernelFiber::KernelFiber(AbiParameters abi)
: _executor{&_context, abi} { }

} // namespace thor

