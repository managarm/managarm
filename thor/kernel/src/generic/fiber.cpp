
#include "kernel.hpp"
#include "fiber.hpp"

namespace thor {

void KernelFiber::blockCurrent(frigg::CallbackPtr<bool()> predicate) {
	auto this_fiber = thisFiber();
	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_fiber->_mutex);
	
	if(!predicate())
		return;

	assert(!this_fiber->_blocked);
	this_fiber->_blocked = true;
	getCpuData()->executorContext = nullptr;
	getCpuData()->activeFiber = nullptr;

	forkExecutor([&] {
		Scheduler::suspend(this_fiber);
		runDetached([] (frigg::LockGuard<frigg::TicketLock> lock) {
			lock.unlock();
			localScheduler()->reschedule();
		}, frigg::move(lock));
	}, &this_fiber->_executor);
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

void KernelFiber::run(UniqueKernelStack stack,
		void (*function)(void *), void *argument) {
	AbiParameters params;
	params.ip = (uintptr_t)function;
	params.argument = (uintptr_t)argument;

	auto fiber = frigg::construct<KernelFiber>(*kernelAlloc, std::move(stack), params);
	Scheduler::associate(fiber, localScheduler());
	Scheduler::resume(fiber);
}

KernelFiber *KernelFiber::post(UniqueKernelStack stack,
		void (*function)(void *), void *argument) {
	AbiParameters params;
	params.ip = (uintptr_t)function;
	params.argument = (uintptr_t)argument;

	auto fiber = frigg::construct<KernelFiber>(*kernelAlloc, std::move(stack), params);
	Scheduler::associate(fiber, localScheduler());
	return fiber;
}

KernelFiber::KernelFiber(UniqueKernelStack stack, AbiParameters abi)
: _blocked{false}, _fiberContext{std::move(stack)}, _executor{&_fiberContext, abi} { }

void KernelFiber::invoke() {
	assert(!intsAreEnabled());

	getCpuData()->executorContext = &_executorContext;
	getCpuData()->activeFiber = this;
	restoreExecutor(&_executor);
}

void KernelFiber::unblock() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	if(!_blocked)
		return;
	
	_blocked = false;
	Scheduler::resume(this);
}

void KernelFiber::AssociatedWorkQueue::wakeup() {
	auto self = frg::container_of(this, &KernelFiber::_associatedWorkQueue);
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&self->_mutex);

	if(!self->_blocked)
		return;

	self->_blocked = false;
	Scheduler::resume(self);
}

KernelFiber *thisFiber() {
	return getCpuData()->activeFiber;
}

} // namespace thor

