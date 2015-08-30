
#include "kernel.hpp"

namespace traits = frigg::traits;

namespace thor {

frigg::util::LazyInitializer<ThreadQueue> activeList;
frigg::util::LazyInitializer<ScheduleQueue> scheduleQueue;
frigg::util::LazyInitializer<ScheduleLock> scheduleLock;

KernelUnsafePtr<Thread> getCurrentThread() {
	return getCpuContext()->currentThread;
}

void resetCurrentThread() {
	ASSERT(!intsAreEnabled());
	auto cpu_context = getCpuContext();
	ASSERT(cpu_context->currentThread);

	cpu_context->currentThread->deactivate();
	cpu_context->currentThread = KernelUnsafePtr<Thread>();
}

void dropCurrentThread() {
	ASSERT(!intsAreEnabled());
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	resetCurrentThread();
	activeList->remove(this_thread);
	
	ScheduleGuard schedule_guard(scheduleLock.get());
	doSchedule(traits::move(schedule_guard));
	// note: doSchedule takes care of the lock
}

void enterThread(KernelUnsafePtr<Thread> thread) {
	ASSERT(!intsAreEnabled());
	auto cpu_context = getCpuContext();
	ASSERT(!cpu_context->currentThread);

	thread->activate();
	cpu_context->currentThread = thread;
	restoreThisThread();
}

void doSchedule(ScheduleGuard &&guard) {
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

void enqueueInSchedule(ScheduleGuard &guard, KernelUnsafePtr<Thread> thread) {
	ASSERT(guard.protects(scheduleLock.get()));

	scheduleQueue->addBack(thread);
}

} // namespace thor

