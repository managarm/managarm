
#include <atomic>

namespace thor {
	
enum EventType {
	kEventNone,
	kEventMemoryLoad,
	kEventMemoryLock,
	kEventObserve
};

struct AsyncEvent {
	AsyncEvent();

	AsyncEvent(EventType type, SubmitInfo submit_info);

	EventType type;
	SubmitInfo submitInfo;

	// used by kEventRecvStringError
	Error error;

	// used by kEventMemoryLoad, kEventRecvStringTransferToBuffer
	// and kEventRecvStringTransferToQueue
	size_t offset;
	size_t length;
	
	// used by kEventRecvStringTransferToBuffer, kEventRecvStringTransferToQueue
	// and kEventRecvDescriptor
	int64_t msgRequest;
	int64_t msgSequence;

	// used by kEventRecvDescriptor, kEventAccept, kEventConnect
	Handle handle;
};

typedef frigg::Variant<
	NullCompleter,
	PostEventCompleter,
	ReturnFromForkCompleter
> AsyncCompleter;

struct AsyncOperation {
	static void complete(frigg::SharedPtr<AsyncOperation> operation);

	AsyncOperation(AsyncCompleter completer)
	: completer(frigg::move(completer)), isComplete(false) { }

	virtual AsyncEvent getEvent() = 0;

	AsyncCompleter completer;
	
	std::atomic<bool> isComplete;
	
	frigg::IntrusiveSharedLinkedItem<AsyncOperation> hubItem;
};

struct AsyncHandleLoad : public AsyncOperation {
	AsyncHandleLoad(AsyncCompleter completer)
	: AsyncOperation(frigg::move(completer)) { }
	
	AsyncEvent getEvent() override;
	
	frigg::IntrusiveSharedLinkedItem<AsyncHandleLoad> processQueueItem;

	size_t offset;
	size_t length;
};

struct AsyncInitiateLoad : public AsyncOperation {
	AsyncInitiateLoad(AsyncCompleter completer, size_t offset, size_t length)
	: AsyncOperation(frigg::move(completer)), offset(offset), length(length), progress(0) { }

	size_t offset;
	size_t length;

	// byte offset for which AsyncHandleLoads have already been issued
	size_t progress;
	
	AsyncEvent getEvent() override;
	
	frigg::IntrusiveSharedLinkedItem<AsyncInitiateLoad> processQueueItem;
};

struct AsyncObserve : public AsyncOperation {
	AsyncObserve(AsyncCompleter completer)
	: AsyncOperation(frigg::move(completer)) { }
	
	AsyncEvent getEvent() override;
	
	frigg::IntrusiveSharedLinkedItem<AsyncObserve> processQueueItem;
};

struct AsyncWaitForEvent : public AsyncOperation {
	AsyncWaitForEvent(AsyncCompleter completer, int64_t filter_async_id)
	: AsyncOperation(frigg::move(completer)), filterAsyncId(filter_async_id) { }
	
	AsyncEvent getEvent() override;

	int64_t filterAsyncId;
	
	frigg::IntrusiveSharedLinkedItem<AsyncWaitForEvent> processQueueItem;

	AsyncEvent event;
};

class EventHub {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	EventHub();

	void raiseEvent(Guard &guard, frigg::SharedPtr<AsyncOperation> operation);

	void submitWaitForEvent(Guard &guard, frigg::SharedPtr<AsyncWaitForEvent> wait);

	Lock lock;

private:	
	frigg::IntrusiveSharedLinkedList<
		AsyncOperation,
		&AsyncOperation::hubItem
	> _eventQueue;

	frigg::IntrusiveSharedLinkedList<
		AsyncWaitForEvent,
		&AsyncWaitForEvent::processQueueItem
	> _waitQueue;
};

} // namespace thor

