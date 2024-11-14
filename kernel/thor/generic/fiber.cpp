#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/arch-generic/cpu.hpp>

namespace thor {

initgraph::Stage *getFibersAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.fibers-available"};
	return &s;
}

void KernelFiber::blockCurrent(FiberBlocker *blocker) {
	auto this_fiber = thisFiber();
	while(true) {
		// Run the WQ outside of the locks.
		this_fiber->_associatedWorkQueue->run();

		StatelessIrqLock irq_lock;
		auto lock = frg::guard(&this_fiber->_mutex);
		
		// Those are the important tests; they are protected by the fiber's mutex.
		if(blocker->_done)
			break;
		if(this_fiber->_associatedWorkQueue->check())
			continue;

		assert(!this_fiber->_blocked);
		this_fiber->_blocked = true;
		getCpuData()->executorContext = nullptr;
		getCpuData()->activeFiber = nullptr;
		getCpuData()->scheduler.update();
		Scheduler::suspendCurrent();
		localScheduler()->forceReschedule();

		forkExecutor([&] {
			runOnStack([] (Continuation cont, Executor *executor,
					frg::unique_lock<frg::ticket_spinlock> lock) {
				scrubStack(executor, cont);
				lock.unlock();
				localScheduler()->commitReschedule();
			}, getCpuData()->detachedStack.base(), &this_fiber->_executor, std::move(lock));
		}, &this_fiber->_executor);
	}
}

void KernelFiber::exitCurrent() {
	infoLogger() << "thor: Fix exiting fibers" << frg::endlog;

	FiberBlocker blocker;
	blocker.setup();
	KernelFiber::blockCurrent(&blocker);
}

void KernelFiber::unblockOther(FiberBlocker *blocker) {
	auto fiber = blocker->_fiber;
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&fiber->_mutex);

	assert(!blocker->_done);
	blocker->_done = true;

	if(!fiber->_blocked)
		return;
	
	fiber->_blocked = false;
	Scheduler::resume(fiber);
}

void KernelFiber::run(UniqueKernelStack stack,
		void (*function)(void *), void *argument, Scheduler *scheduler) {
	AbiParameters params;
	params.ip = (uintptr_t)function;
	params.argument = (uintptr_t)argument;

	auto fiber = frg::construct<KernelFiber>(*kernelAlloc, std::move(stack), params);
	Scheduler::associate(fiber, scheduler);
	Scheduler::resume(fiber);
}

KernelFiber *KernelFiber::post(UniqueKernelStack stack,
		void (*function)(void *), void *argument, Scheduler *scheduler) {
	AbiParameters params;
	params.ip = (uintptr_t)function;
	params.argument = (uintptr_t)argument;

	auto fiber = frg::construct<KernelFiber>(*kernelAlloc, std::move(stack), params);
	Scheduler::associate(fiber, scheduler);
	return fiber;
}

KernelFiber::KernelFiber(UniqueKernelStack stack, AbiParameters abi)
: _blocked{false}, _fiberContext{std::move(stack)}, _executor{&_fiberContext, abi} {
	_associatedWorkQueue = smarter::allocate_shared<AssociatedWorkQueue>(*kernelAlloc, this);
	_associatedWorkQueue->selfPtr = smarter::shared_ptr<WorkQueue>{_associatedWorkQueue};
}

void KernelFiber::invoke() {
	assert(!intsAreEnabled());

	getCpuData()->executorContext = &_executorContext;
	getCpuData()->activeFiber = this;
	restoreExecutor(&_executor);
}

void KernelFiber::handlePreemption(IrqImageAccessor) {
	// Do nothing (do not preempt fibers for now).
}

void KernelFiber::AssociatedWorkQueue::wakeup() {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&fiber_->_mutex);

	if(!fiber_->_blocked)
		return;

	fiber_->_blocked = false;
	Scheduler::resume(fiber_);
}

void FiberBlocker::setup() {
	_fiber = thisFiber();
	_done = false;
}

KernelFiber *thisFiber() {
	return getCpuData()->activeFiber;
}

} // namespace thor

