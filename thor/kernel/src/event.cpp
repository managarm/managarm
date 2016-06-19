
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// UserEvent
// --------------------------------------------------------

UserEvent::UserEvent(Type type, SubmitInfo submit_info)
		: type(type), submitInfo(submit_info) { }

UserEvent AsyncSendString::getEvent() { assert(false); }
UserEvent AsyncSendDescriptor::getEvent() {
	UserEvent event(UserEvent::kTypeSendDescriptor, submitInfo);
	event.error = kErrSuccess;
	return event;
}
UserEvent AsyncRecvString::getEvent() { assert(false); }
UserEvent AsyncRecvDescriptor::getEvent() {
	UserEvent event(UserEvent::kTypeRecvDescriptor, submitInfo);
	event.error = error;
	event.msgRequest = msgRequest;
	event.msgSequence = msgSequence;
	event.descriptor = frigg::move(descriptor);
	return event;
}
UserEvent AsyncAccept::getEvent() { assert(false); }
UserEvent AsyncConnect::getEvent() { assert(false); }
UserEvent AsyncRingItem::getEvent() { assert(false); }

// --------------------------------------------------------
// EventHub
// --------------------------------------------------------

EventHub::EventHub() : p_waitingThreads(*kernelAlloc) { }

void EventHub::raiseEvent(Guard &guard, frigg::SharedPtr<AsyncOperation> operation) {
	assert(guard.protects(&lock));

	_eventQueue.addBack(frigg::move(operation));

	while(!p_waitingThreads.empty()) {
		KernelSharedPtr<Thread> waiting = p_waitingThreads.removeFront().grab();

		ScheduleGuard schedule_guard(scheduleLock.get());
		enqueueInSchedule(schedule_guard, waiting);
		schedule_guard.unlock();
	}
}

bool EventHub::hasEvent(Guard &guard) {
	assert(guard.protects(&lock));

	return !_eventQueue.empty();
}

frigg::SharedPtr<AsyncOperation> EventHub::dequeueEvent(Guard &guard) {
	assert(guard.protects(&lock));

	return _eventQueue.removeFront();
}

void EventHub::blockCurrentThread(Guard &guard) {
	assert(!intsAreEnabled());
	assert(guard.protects(&lock));

	void *restore_state = __builtin_alloca(getStateSize());
	if(forkState(restore_state)) {
		KernelUnsafePtr<Thread> this_thread = getCurrentThread();
		p_waitingThreads.addBack(this_thread.toWeak());
		
		// keep the lock on this hub unlocked while we sleep
		guard.unlock();
		
		resetCurrentThread(restore_state);
		
		ScheduleGuard schedule_guard(scheduleLock.get());
		doSchedule(frigg::move(schedule_guard));
		// note: doSchedule() takes care of the schedule_guard lock
	}
	
	// the guard lock was released during the first return of saveThisThread()
	guard.lock();
}

} // namespace thor

