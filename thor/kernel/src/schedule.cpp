
#include "kernel.hpp"

namespace thor {

frigg::LazyInitializer<ThreadQueue> activeList;
frigg::LazyInitializer<ScheduleQueue> scheduleQueue;
frigg::LazyInitializer<ScheduleLock> scheduleLock;

KernelUnsafePtr<Thread> getCurrentThread() {
	return getCpuContext()->currentThread;
}

void resetCurrentThread(void *restore_state) {
	assert(!intsAreEnabled());
	auto cpu_context = getCpuContext();
	assert(cpu_context->currentThread);
	
	assert(!cpu_context->currentThread->accessSaveState().restoreState);
	cpu_context->currentThread->accessSaveState().restoreState = restore_state;

	cpu_context->currentThread->deactivate();
	cpu_context->currentThread = KernelUnsafePtr<Thread>();
}

void dropCurrentThread() {
	assert(!intsAreEnabled());
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	resetCurrentThread(nullptr);
	activeList->remove(this_thread);
	
	ScheduleGuard schedule_guard(scheduleLock.get());
	doSchedule(frigg::move(schedule_guard));
	// note: doSchedule takes care of the lock
}

void enterThread(KernelUnsafePtr<Thread> thread) {
	assert(!intsAreEnabled());
	auto cpu_context = getCpuContext();
	assert(!cpu_context->currentThread);

//FIXME: re-enable preemption
//	if((thread->flags & Thread::kFlagExclusive) == 0)
//		preemptThisCpu(100000000);

	thread->activate();
	cpu_context->currentThread = thread;
	void *restore_state = thread->accessSaveState().restoreState;
	assert(restore_state);
	thread->accessSaveState().restoreState = nullptr;
	restoreStateFrame(restore_state);
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

extern "C" void onPreemption(void *state) {
	acknowledgePreemption();
	
	KernelUnsafePtr<Thread> thread = getCurrentThread();
	resetCurrentThread(state);
	
	ScheduleGuard schedule_guard(scheduleLock.get());
	if((thread->flags & Thread::kFlagNotScheduled) == 0)
		enqueueInSchedule(schedule_guard, thread);
	doSchedule(frigg::move(schedule_guard));
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

