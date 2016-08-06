
#include "kernel.hpp"

namespace thor {

frigg::LazyInitializer<ScheduleQueue> scheduleQueue;
frigg::LazyInitializer<ScheduleLock> scheduleLock;

KernelUnsafePtr<Thread> getCurrentThread() {
	return activeExecutor();
}

void doSchedule(ScheduleGuard &&guard) {
	assert(!intsAreEnabled());
	
	// this function might destroy the currrent thread and thus deallocate
	// its kernel stack; make sure we're running from a per-cpu stack.
	runDetached([] (ScheduleGuard guard) {
		assert(guard.protects(scheduleLock.get()));
	
		// TODO: this should be replaced by a proper "reschedule after IRQ" strategy
		while(scheduleQueue->empty()) {
			guard.unlock();
			enableInts();

			halt();

			disableInts();
			guard.lock();
		}

		KernelUnsafePtr<Thread> thread = scheduleQueue->removeFront();
		
		guard.unlock();
		switchExecutor(thread);
		restoreExecutor();
	}, frigg::move(guard));
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

} // namespace thor

