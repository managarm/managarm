
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// SubmitInfo
// --------------------------------------------------------

SubmitInfo::SubmitInfo(int64_t async_id,
		uintptr_t submit_function, uintptr_t submit_object)
	: asyncId(async_id), submitFunction(submit_function),
		submitObject(submit_object) { }

// --------------------------------------------------------
// UserEvent
// --------------------------------------------------------

UserEvent::UserEvent(Type type, SubmitInfo submit_info)
		: type(type), submitInfo(submit_info) { }

// --------------------------------------------------------
// EventHub
// --------------------------------------------------------

EventHub::EventHub() : p_queue(*kernelAlloc), p_waitingThreads(*kernelAlloc) { }

void EventHub::raiseEvent(Guard &guard, UserEvent &&event) {
	assert(guard.protects(&lock));

	p_queue.addBack(frigg::move(event));

	while(!p_waitingThreads.empty()) {
		KernelSharedPtr<Thread> waiting(p_waitingThreads.removeFront());

		ScheduleGuard schedule_guard(scheduleLock.get());
		enqueueInSchedule(schedule_guard, waiting);
		schedule_guard.unlock();
	}
}

bool EventHub::hasEvent(Guard &guard) {
	assert(guard.protects(&lock));

	return !p_queue.empty();
}

UserEvent EventHub::dequeueEvent(Guard &guard) {
	assert(guard.protects(&lock));

	return p_queue.removeFront();
}

void EventHub::blockCurrentThread(Guard &guard) {
	assert(guard.protects(&lock));

	assert(!intsAreEnabled());
	if(saveThisThread()) {
		KernelUnsafePtr<Thread> this_thread = getCurrentThread();
		p_waitingThreads.addBack(KernelWeakPtr<Thread>(this_thread));
		
		// keep the lock on this hub unlocked while we sleep
		guard.unlock();
		
		resetCurrentThread();
		
		ScheduleGuard schedule_guard(scheduleLock.get());
		doSchedule(frigg::move(schedule_guard));
		// note: doSchedule() takes care of the schedule_guard lock
	}
	
	// the guard lock was released during the first return of saveThisThread()
	guard.lock();
}

} // namespace thor

