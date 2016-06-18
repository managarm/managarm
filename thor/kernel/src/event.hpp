
namespace thor {

struct AsyncOperation {
	AsyncOperation(AsyncData data)
	: eventHub(frigg::move(data.eventHub)),
		submitInfo(data.asyncId, data.submitFunction, data.submitObject) { }

	frigg::WeakPtr<EventHub> eventHub;
	SubmitInfo submitInfo;
};

// TODO: clean this up; split this into Send/Recv type
enum MsgType {
	kMsgNone,
	kMsgString,
	kMsgStringToBuffer,
	kMsgStringToRing,
	kMsgDescriptor
};

struct AsyncSendString : public AsyncOperation {
	AsyncSendString(AsyncData data, MsgType type,
			int64_t msg_request, int64_t msg_sequence)
	: AsyncOperation(frigg::move(data)), type(type),
			msgRequest(msg_request), msgSequence(msg_sequence), flags(0) { }
	
	MsgType type;
	frigg::UniqueMemory<KernelAlloc> kernelBuffer;
	AnyDescriptor descriptor;
	int64_t msgRequest;
	int64_t msgSequence;
	uint32_t flags;

	frigg::IntrusiveSharedLinkedItem<AsyncSendString> sendItem;
};

struct AsyncRecvString : public AsyncOperation {
	AsyncRecvString(AsyncData data, MsgType type,
			int64_t filter_request, int64_t filter_sequence)
	: AsyncOperation(frigg::move(data)), type(type),
			filterRequest(filter_request), filterSequence(filter_sequence) { }
	
	MsgType type;
	int64_t filterRequest;
	int64_t filterSequence;
	uint32_t flags;
	
	// used by kMsgStringToBuffer
	ForeignSpaceLock spaceLock;
	
	// used by kMsgStringToRing
	frigg::SharedPtr<RingBuffer> ringBuffer;
	
	frigg::IntrusiveSharedLinkedItem<AsyncRecvString> recvItem;
};

struct AsyncAccept : public AsyncOperation {
	AsyncAccept(AsyncData data)
	: AsyncOperation(frigg::move(data)) { }
	
	frigg::IntrusiveSharedLinkedItem<AsyncAccept> processItem;
};

struct AsyncConnect : public AsyncOperation {
	AsyncConnect(AsyncData data)
	: AsyncOperation(frigg::move(data)) { }
	
	frigg::IntrusiveSharedLinkedItem<AsyncConnect> processItem;
};

struct AsyncRingItem : public AsyncOperation {
	AsyncRingItem(AsyncData data, DirectSpaceLock<HelRingBuffer> space_lock,
			size_t buffer_size);

	DirectSpaceLock<HelRingBuffer> spaceLock;
	size_t bufferSize;

	size_t offset;

	frigg::IntrusiveSharedLinkedItem<AsyncRingItem> bufferItem;
};

struct UserEvent {
	enum Type {
		kTypeNone,
		kTypeError,
		kTypeMemoryLoad,
		kTypeMemoryLock,
		kTypeJoin,
		kTypeSendString,
		kTypeSendDescriptor,
		// TODO: use only a single kTypeRecvString
		kTypeRecvStringTransferToBuffer,
		kTypeRecvStringTransferToQueue,
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

	// used by kTypeAccept, kTypeConnect
	KernelSharedPtr<Endpoint> endpoint;

	// used by kTypeRecvDescriptor
	AnyDescriptor descriptor;
};

class EventHub {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	EventHub();

	void raiseEvent(Guard &guard, UserEvent &&event);

	bool hasEvent(Guard &guard);

	UserEvent dequeueEvent(Guard &guard);

	void blockCurrentThread(Guard &guard);

	Lock lock;

private:
	frigg::LinkedList<UserEvent, KernelAlloc> p_queue;
	frigg::LinkedList<KernelWeakPtr<Thread>, KernelAlloc> p_waitingThreads;
};

} // namespace thor

