
namespace thor {

struct UserEvent {
	enum Type {
		kTypeNone,
		kTypeError,
		kTypeMemoryLoad,
		kTypeMemoryLock,
		kTypeJoin,
		kTypeSendString,
		kTypeSendDescriptor,
		kTypeRecvString,
		kTypeRecvStringToRing,
		kTypeRecvDescriptor,
		kTypeAccept,
		kTypeConnect,
		kTypeIrq
	};

	UserEvent(Type type, SubmitInfo submit_info);

	Type type;
	SubmitInfo submitInfo;

	// used by kTypeRecvStringError
	Error error;

	// used by kTypeMemoryLoad, kTypeRecvStringTransferToBuffer
	// and kTypeRecvStringTransferToQueue
	size_t offset;
	size_t length;
	
	// used by kTypeRecvStringTransferToBuffer, kTypeRecvStringTransferToQueue
	// and kTypeRecvDescriptor
	int64_t msgRequest;
	int64_t msgSequence;

	// used by kTypeRecvDescriptor, kTypeAccept, kTypeConnect
	Handle handle;
};

struct AsyncOperation {
	static void complete(frigg::SharedPtr<AsyncOperation> operation);

	AsyncOperation(AsyncData data)
	: eventHub(frigg::move(data.eventHub)),
		submitInfo(data.asyncId, data.submitFunction, data.submitObject) { }

	virtual UserEvent getEvent() = 0;

	frigg::WeakPtr<EventHub> eventHub;
	SubmitInfo submitInfo;
	
	frigg::IntrusiveSharedLinkedItem<AsyncOperation> hubItem;
};

struct AsyncSendString : public AsyncOperation {
	AsyncSendString(AsyncData data, int64_t msg_request, int64_t msg_sequence)
	: AsyncOperation(frigg::move(data)), msgRequest(msg_request), msgSequence(msg_sequence),
			flags(0) { }
	
	UserEvent getEvent() override;
	
	frigg::UniqueMemory<KernelAlloc> kernelBuffer;
	int64_t msgRequest;
	int64_t msgSequence;
	uint32_t flags;

	frigg::IntrusiveSharedLinkedItem<AsyncSendString> processQueueItem;
};

struct AsyncSendDescriptor : public AsyncOperation {
	AsyncSendDescriptor(AsyncData data, int64_t msg_request, int64_t msg_sequence)
	: AsyncOperation(frigg::move(data)), msgRequest(msg_request), msgSequence(msg_sequence),
			flags(0) { }
	
	UserEvent getEvent() override;
	
	AnyDescriptor descriptor;
	int64_t msgRequest;
	int64_t msgSequence;
	uint32_t flags;
	

	frigg::IntrusiveSharedLinkedItem<AsyncSendDescriptor> processQueueItem;
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
	
	UserEvent getEvent() override;
	
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
	
	UserEvent getEvent() override;
	
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
	
	UserEvent getEvent() override;
	
	frigg::WeakPtr<Universe> universe;
	
	frigg::IntrusiveSharedLinkedItem<AsyncAccept> processItem;

	Handle handle;
};

struct AsyncConnect : public AsyncOperation {
	AsyncConnect(AsyncData data, frigg::WeakPtr<Universe> universe)
	: AsyncOperation(frigg::move(data)), universe(frigg::move(universe)) { }
	
	UserEvent getEvent() override;
	
	frigg::WeakPtr<Universe> universe;
	
	frigg::IntrusiveSharedLinkedItem<AsyncConnect> processItem;

	Handle handle;
};

struct AsyncRingItem : public AsyncOperation {
	AsyncRingItem(AsyncData data, DirectSpaceLock<HelRingBuffer> space_lock,
			size_t buffer_size);
	
	UserEvent getEvent() override;

	DirectSpaceLock<HelRingBuffer> spaceLock;
	size_t bufferSize;

	size_t offset;

	frigg::IntrusiveSharedLinkedItem<AsyncRingItem> bufferItem;
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

