
#include "kernel.hpp"

namespace thor {

frigg::LazyInitializer<ThreadQueue> activeList;
frigg::LazyInitializer<ScheduleQueue> scheduleQueue;
frigg::LazyInitializer<ScheduleLock> scheduleLock;

KernelUnsafePtr<Thread> getCurrentThread() {
	return activeExecutor();
}

void dropCurrentThread() {
	assert(!"Fix dropCurrentThread");
}

void enterThread(KernelUnsafePtr<Thread> thread) {
	switchExecutor(thread);
	restoreExecutor();
}

void doSchedule(ScheduleGuard &&guard) {
	assert(!intsAreEnabled());
	assert(guard.protects(scheduleLock.get()));

	// FIXME: make sure that we only schedule from
	// a executor-indepedent domain.
	
	if(!scheduleQueue->empty()) {
		KernelUnsafePtr<Thread> thread = scheduleQueue->removeFront();
		
		guard.unlock();
		enterThread(thread);
	}else{
		guard.unlock();
		enterThread(getCpuContext()->idleThread);
	}
}

// FIXME: this function should get a parameter of type IrqImagePtr
extern "C" void onPreemption() {
	assert(!"Fix preemption");
/*	acknowledgePreemption();
	
	KernelUnsafePtr<Thread> thread = getCurrentThread();
	resetCurrentThread(state);
	
	ScheduleGuard schedule_guard(scheduleLock.get());
	if((thread->flags & Thread::kFlagNotScheduled) == 0)
		enqueueInSchedule(schedule_guard, thread);
	doSchedule(frigg::move(schedule_guard));*/
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

