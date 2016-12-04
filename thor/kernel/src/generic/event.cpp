
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// AsyncEvent
// --------------------------------------------------------

AsyncEvent::AsyncEvent()
: type(kEventNone) { }

AsyncEvent::AsyncEvent(EventType type, SubmitInfo submit_info)
: type(type), submitInfo(submit_info) { }

AsyncEvent AsyncHandleLoad::getEvent() {
	AsyncEvent event(kEventMemoryLoad, completer.get<PostEventCompleter>().submitInfo);
	event.error = kErrSuccess;
	event.offset = offset;
	event.length = length;
	return event;
}
AsyncEvent AsyncInitiateLoad::getEvent() {
	AsyncEvent event(kEventMemoryLock, completer.get<PostEventCompleter>().submitInfo);
	event.error = kErrSuccess;
	return event;
}
AsyncEvent AsyncObserve::getEvent() {
	AsyncEvent event(kEventObserve, completer.get<PostEventCompleter>().submitInfo);
	event.error = kErrSuccess;
	return event;
}
AsyncEvent AsyncWaitForEvent::getEvent() {
	assert(false);
	__builtin_trap();
}

// --------------------------------------------------------
// AsyncOperation
// --------------------------------------------------------

void AsyncOperation::complete(frigg::SharedPtr<AsyncOperation> operation) {
	operation->isComplete.store(true, std::memory_order_release);

	AsyncCompleter &completer = operation->completer;
	switch(completer.tag()) {
	case AsyncCompleter::tagOf<NullCompleter>():
		break;
	case AsyncCompleter::tagOf<PostEventCompleter>(): {
		auto event_hub = completer.get<PostEventCompleter>().eventHub.grab();
		assert(event_hub);

		EventHub::Guard hub_guard(&event_hub->lock);
		event_hub->raiseEvent(hub_guard, frigg::move(operation));
	} break;
	case AsyncCompleter::tagOf<ReturnFromForkCompleter>(): {
		auto thread = completer.get<ReturnFromForkCompleter>().thread.grab();
		assert(thread);

		Thread::unblockOther(thread);
	} break;
	default:
		assert(!"Unexpected AsyncCompleter");
	}
}

// --------------------------------------------------------
// EventHub
// --------------------------------------------------------

EventHub::EventHub() { }

void EventHub::raiseEvent(Guard &guard, frigg::SharedPtr<AsyncOperation> operation) {
	assert(guard.protects(&lock));

	for(auto it = _waitQueue.frontIter(); it; ) {
		auto it_copy = it++;

		auto submit_info = operation->completer.get<PostEventCompleter>().submitInfo;
		if((*it_copy)->filterAsyncId == -1 || (*it_copy)->filterAsyncId == submit_info.asyncId) {
			auto wait = _waitQueue.remove(it_copy);
			wait->event = operation->getEvent();
			AsyncOperation::complete(frigg::move(wait));
			return;
		}
	}

	_eventQueue.addBack(frigg::move(operation));
}

void EventHub::submitWaitForEvent(Guard &guard, frigg::SharedPtr<AsyncWaitForEvent> wait) {
	assert(guard.protects(&lock));

	for(auto it = _eventQueue.frontIter(); it; ) {
		auto it_copy = it++;

		auto submit_info = (*it_copy)->completer.get<PostEventCompleter>().submitInfo;
		if(wait->filterAsyncId == -1 || wait->filterAsyncId == submit_info.asyncId) {
			auto operation = _eventQueue.remove(it_copy);
			wait->event = operation->getEvent();
			AsyncOperation::complete(frigg::move(wait));
			return;
		}
	}

	_waitQueue.addBack(frigg::move(wait));
}

} // namespace thor

