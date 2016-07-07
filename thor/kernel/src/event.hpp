
namespace thor {
	
enum EventType {
	kEventNone,
	kEventMemoryLoad,
	kEventMemoryLock,
	kEventObserve,
	kEventSendString,
	kEventSendDescriptor,
	kEventRecvString,
	kEventRecvStringToRing,
	kEventRecvDescriptor,
	kEventAccept,
	kEventConnect,
	kEventIrq
};

struct AsyncEvent {
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

struct AsyncOperation {
	static void complete(frigg::SharedPtr<AsyncOperation> operation);

	AsyncOperation(AsyncData data)
	: eventHub(frigg::move(data.eventHub)),
		submitInfo(data.asyncId, data.submitFunction, data.submitObject) { }

	virtual AsyncEvent getEvent() = 0;

	frigg::WeakPtr<EventHub> eventHub;
	SubmitInfo submitInfo;
	
	frigg::IntrusiveSharedLinkedItem<AsyncOperation> hubItem;
};

struct AsyncHandleLoad : public AsyncOperation {
	AsyncHandleLoad(AsyncData data)
	: AsyncOperation(frigg::move(data)) { }
	
	AsyncEvent getEvent() override;
	
	frigg::IntrusiveSharedLinkedItem<AsyncHandleLoad> processQueueItem;

	size_t offset;
	size_t length;
};

struct AsyncInitiateLoad : public AsyncOperation {
	AsyncInitiateLoad(AsyncData data, size_t offset, size_t length)
	: AsyncOperation(frigg::move(data)), offset(offset), length(length), progress(0) { }

	size_t offset;
	size_t length;

	// byte offset for which AsyncHandleLoads have already been issued
	size_t progress;
	
	AsyncEvent getEvent() override;
	
	frigg::IntrusiveSharedLinkedItem<AsyncInitiateLoad> processQueueItem;
};

struct AsyncObserve : public AsyncOperation {
	AsyncObserve(AsyncData data)
	: AsyncOperation(frigg::move(data)) { }
	
	AsyncEvent getEvent() override;
	
	frigg::IntrusiveSharedLinkedItem<AsyncObserve> processQueueItem;
};

struct AsyncSendString : public AsyncOperation {
	AsyncSendString(AsyncData data, int64_t msg_request, int64_t msg_sequence)
	: AsyncOperation(frigg::move(data)), msgRequest(msg_request), msgSequence(msg_sequence),
			flags(0) { }
	
	AsyncEvent getEvent() override;
	
	frigg::UniqueMemory<KernelAlloc> kernelBuffer;
	int64_t msgRequest;
	int64_t msgSequence;
	uint32_t flags;

	frigg::IntrusiveSharedLinkedItem<AsyncSendString> processQueueItem;
	
	Error error;
};

struct AsyncSendDescriptor : public AsyncOperation {
	AsyncSendDescriptor(AsyncData data, int64_t msg_request, int64_t msg_sequence)
	: AsyncOperation(frigg::move(data)), msgRequest(msg_request), msgSequence(msg_sequence),
			flags(0) { }
	
	AsyncEvent getEvent() override;
	
	AnyDescriptor descriptor;
	int64_t msgRequest;
	int64_t msgSequence;
	uint32_t flags;

	frigg::IntrusiveSharedLinkedItem<AsyncSendDescriptor> processQueueItem;
	
	Error error;
};

struct AsyncRecvString : public AsyncOperation {
	enum Type {
		kTypeNormal,
		kTypeToRing
	};

	AsyncRecvString(AsyncData data, Type type,
			int64_t filter_request, int64_t filter_sequence)
	: AsyncOperation(frigg::move(data)), type(type),
			filterRequest(filter_request), filterSequence(filter_sequence) { }
	
	AsyncEvent getEvent() override;
	
	Type type;
	int64_t filterRequest;
	int64_t filterSequence;
	uint32_t flags;
	
	// used by kTypeNormal
	ForeignSpaceLock spaceLock;
	
	// used by kTypeToRing
	frigg::SharedPtr<RingBuffer> ringBuffer;
	
	frigg::IntrusiveSharedLinkedItem<AsyncRecvString> processQueueItem;

	Error error;
	int64_t msgRequest;
	int64_t msgSequence;
	size_t offset;
	size_t length;
};

struct AsyncRecvDescriptor : public AsyncOperation {
	AsyncRecvDescriptor(AsyncData data, frigg::WeakPtr<Universe> universe,
			int64_t filter_request, int64_t filter_sequence)
	: AsyncOperation(frigg::move(data)), universe(frigg::move(universe)),
			filterRequest(filter_request), filterSequence(filter_sequence) { }
	
	AsyncEvent getEvent() override;
	
	frigg::WeakPtr<Universe> universe;
	int64_t filterRequest;
	int64_t filterSequence;
	uint32_t flags;
	
	frigg::IntrusiveSharedLinkedItem<AsyncRecvDescriptor> processQueueItem;

	Error error;
	int64_t msgRequest;
	int64_t msgSequence;
	Handle handle;
};

struct AsyncAccept : public AsyncOperation {
	AsyncAccept(AsyncData data, frigg::WeakPtr<Universe> universe)
	: AsyncOperation(frigg::move(data)), universe(frigg::move(universe)) { }
	
	AsyncEvent getEvent() override;
	
	frigg::WeakPtr<Universe> universe;
	
	frigg::IntrusiveSharedLinkedItem<AsyncAccept> processItem;

	Handle handle;
};

struct AsyncConnect : public AsyncOperation {
	AsyncConnect(AsyncData data, frigg::WeakPtr<Universe> universe)
	: AsyncOperation(frigg::move(data)), universe(frigg::move(universe)) { }
	
	AsyncEvent getEvent() override;
	
	frigg::WeakPtr<Universe> universe;
	
	frigg::IntrusiveSharedLinkedItem<AsyncConnect> processItem;

	Handle handle;
};

struct AsyncRingItem : public AsyncOperation {
	AsyncRingItem(AsyncData data, DirectSpaceLock<HelRingBuffer> space_lock,
			size_t buffer_size);
	
	AsyncEvent getEvent() override;

	DirectSpaceLock<HelRingBuffer> spaceLock;
	size_t bufferSize;

	size_t offset;

	frigg::IntrusiveSharedLinkedItem<AsyncRingItem> bufferItem;
};

struct AsyncIrq : public AsyncOperation {
	AsyncIrq(AsyncData data)
	: AsyncOperation(frigg::move(data)) { }
	
	AsyncEvent getEvent() override;
	
	frigg::WeakPtr<Universe> universe;
	
	frigg::IntrusiveSharedLinkedItem<AsyncIrq> processQueueItem;
};

class EventHub {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	EventHub();

	void raiseEvent(Guard &guard, frigg::SharedPtr<AsyncOperation> operation);

	bool hasEvent(Guard &guard);

	frigg::SharedPtr<AsyncOperation> dequeueEvent(Guard &guard);

	void blockCurrentThread(Guard &guard);

	Lock lock;

private:	
	frigg::IntrusiveSharedLinkedList<
		AsyncOperation,
		&AsyncOperation::hubItem
	> _eventQueue;
	
	frigg::LinkedList<KernelWeakPtr<Thread>, KernelAlloc> p_waitingThreads;
};

} // namespace thor

