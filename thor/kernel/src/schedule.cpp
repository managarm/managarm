
#include "kernel.hpp"

namespace traits = frigg::traits;

namespace thor {

frigg::util::LazyInitializer<ThreadQueue> scheduleQueue;

frigg::util::LazyInitializer<ScheduleLock> scheduleLock;

KernelUnsafePtr<Thread> getCurrentThread() {
	return getCpuContext()->currentThread;
}

KernelSharedPtr<Thread> resetCurrentThread() {
	ASSERT(!intsAreEnabled());
	auto cpu_context = getCpuContext();
	ASSERT(cpu_context->currentThread);

	KernelUnsafePtr<Thread> thread = cpu_context->currentThread;
	thread->deactivate();
	return traits::move(cpu_context->currentThread);
}

void dropCurrentThread() {
	ASSERT(!intsAreEnabled());
	resetCurrentThread();
	
	ScheduleGuard schedule_guard(scheduleLock.get());
	doSchedule(schedule_guard);
	// note: doSchedule takes care of the lock
}

void enterThread(KernelSharedPtr<Thread> &&thread) {
	ASSERT(!intsAreEnabled());
	auto cpu_context = getCpuContext();
	ASSERT(!cpu_context->currentThread);

	thread->activate();
	cpu_context->currentThread = traits::move(thread);
	restoreThisThread();
}

void doSchedule(ScheduleGuard &guard) {
	ASSERT(!intsAreEnabled());
	ASSERT(guard.protects(scheduleLock.get()));
	ASSERT(!getCpuContext()->currentThread);
	
	while(scheduleQueue->empty()) {
		guard.unlock();
		enableInts();
		halt();
		disableInts();
		guard.lock();
	}

	guard.unlock();

	enterThread(scheduleQueue->removeFront());
}

void enqueueInSchedule(ScheduleGuard &guard,
		KernelSharedPtr<Thread> &&thread) {
	ASSERT(guard.protects(scheduleLock.get()));

	scheduleQueue->addBack(traits::move(thread));
}

} // namespace thor

