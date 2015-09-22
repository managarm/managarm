
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
	assert(!intsAreEnabled());
	auto cpu_context = getCpuContext();
	assert(cpu_context->currentThread);

	cpu_context->currentThread->deactivate();
	cpu_context->currentThread = KernelUnsafePtr<Thread>();
}

void dropCurrentThread() {
	assert(!intsAreEnabled());
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	resetCurrentThread();
	activeList->remove(this_thread);
	
	ScheduleGuard schedule_guard(scheduleLock.get());
	doSchedule(traits::move(schedule_guard));
	// note: doSchedule takes care of the lock
}

void enterThread(KernelUnsafePtr<Thread> thread) {
	assert(!intsAreEnabled());
	auto cpu_context = getCpuContext();
	assert(!cpu_context->currentThread);
	
	if((thread->flags & Thread::kFlagExclusive) == 0)
		preemptThisCpu(100000000);

	thread->activate();
	cpu_context->currentThread = thread;
	restoreThisThread();
}

void doSchedule(ScheduleGuard &&guard) {
	assert(!intsAreEnabled());
	assert(guard.protects(scheduleLock.get()));
	assert(!getCpuContext()->currentThread);
	
	if(!scheduleQueue->empty()) {
		KernelUnsafePtr<Thread> thread = scheduleQueue->removeFront();
		
		guard.unlock();
		enterThread(thread);
	}else{
		guard.unlock();
		enterThread(getCpuContext()->idleThread);
	}
}

extern "C" void onPreemption() {
	acknowledgePreemption();
	
	KernelUnsafePtr<Thread> thread = getCurrentThread();
	resetCurrentThread();
	
	ScheduleGuard schedule_guard(scheduleLock.get());
	if((thread->flags & Thread::kFlagNotScheduled) == 0)
		enqueueInSchedule(schedule_guard, thread);
	doSchedule(traits::move(schedule_guard));
}

void enqueueInSchedule(ScheduleGuard &guard, KernelUnsafePtr<Thread> thread) {
	assert(guard.protects(scheduleLock.get()));

	scheduleQueue->addBack(thread);
}

void idleRoutine() {
	while(true) {
		disableInts();
		enableInts();
		assert(intsAreEnabled());
		halt();
	}
}

} // namespace thor

