
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// UserEvent
// --------------------------------------------------------

UserEvent::UserEvent(Type type, SubmitInfo submit_info)
		: type(type), submitInfo(submit_info) { }

UserEvent AsyncSendString::getEvent() {
	UserEvent event(UserEvent::kTypeSendString, submitInfo);
	event.error = kErrSuccess;
	return event;
}
UserEvent AsyncSendDescriptor::getEvent() {
	UserEvent event(UserEvent::kTypeSendDescriptor, submitInfo);
	event.error = kErrSuccess;
	return event;
}
UserEvent AsyncRecvString::getEvent() {
	if(type == kTypeNormal) {
		UserEvent event(UserEvent::kTypeRecvString, submitInfo);
		event.error = error;
		event.msgRequest = msgRequest;
		event.msgSequence = msgSequence;
		event.length = length;
		return event;
	}else{
		assert(type == kTypeToRing);
		
		UserEvent event(UserEvent::kTypeRecvStringToRing, submitInfo);
		event.error = error;
		event.msgRequest = msgRequest;
		event.msgSequence = msgSequence;
		event.offset = offset;
		event.length = length;
		return event;
	}
}
UserEvent AsyncRecvDescriptor::getEvent() {
	UserEvent event(UserEvent::kTypeRecvDescriptor, submitInfo);
	event.error = kErrSuccess;
	event.msgRequest = msgRequest;
	event.msgSequence = msgSequence;
	event.handle = handle;
	return event;
}
UserEvent AsyncAccept::getEvent() {
	UserEvent event(UserEvent::kTypeAccept, submitInfo);
	event.error = kErrSuccess;
	event.handle = handle;
	return event;
}
UserEvent AsyncConnect::getEvent() {
	UserEvent event(UserEvent::kTypeConnect, submitInfo);
	event.error = kErrSuccess;
	event.handle = handle;
	return event;
}
UserEvent AsyncRingItem::getEvent() { assert(false); }

// --------------------------------------------------------
// AsyncOperation
// --------------------------------------------------------

void AsyncOperation::complete(frigg::SharedPtr<AsyncOperation> operation) {
	frigg::SharedPtr<EventHub> event_hub = operation->eventHub.grab();
	assert(event_hub);

	EventHub::Guard hub_guard(&event_hub->lock);
	event_hub->raiseEvent(hub_guard, frigg::move(operation));
}

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

