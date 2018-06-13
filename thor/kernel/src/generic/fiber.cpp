
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

void KernelFiber::blockCurrent(FiberBlocker *blocker) {
	auto this_fiber = thisFiber();
	while(true) {
		// Run the WQ outside of the locks.
		this_fiber->_associatedWorkQueue.run();

		StatelessIrqLock irq_lock;
		auto lock = frigg::guard(&this_fiber->_mutex);
		
		// Those are the important tests; they are protected by the fiber's mutex.
		if(blocker->_done)
			break;
		if(this_fiber->_associatedWorkQueue.check())
			continue;

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

void KernelFiber::unblockOther(FiberBlocker *blocker) {
	auto fiber = blocker->_fiber;
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&fiber->_mutex);

	assert(!blocker->_done);
	blocker->_done = true;

	if(!fiber->_blocked)
		return;
	
	fiber->_blocked = false;
	Scheduler::resume(fiber);
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
: _blocked{false}, _fiberContext{std::move(stack)}, _executor{&_fiberContext, abi} {
	_executorContext.associatedWorkQueue = &_associatedWorkQueue;
}

void KernelFiber::invoke() {
	assert(!intsAreEnabled());

	getCpuData()->executorContext = &_executorContext;
	getCpuData()->activeFiber = this;
	restoreExecutor(&_executor);
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

void FiberBlocker::setup() {
	_fiber = thisFiber();
	_done = false;
}

KernelFiber *thisFiber() {
	return getCpuData()->activeFiber;
}

} // namespace thor

