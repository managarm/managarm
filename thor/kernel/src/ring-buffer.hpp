
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
			int64_t msg_request, int64_t msg_sequence);
	
	MsgType type;
	frigg::UniqueMemory<KernelAlloc> kernelBuffer;
	AnyDescriptor descriptor;
	int64_t msgRequest;
	int64_t msgSequence;
	uint32_t flags;

	frigg::IntrusiveSharedLinkedItem<AsyncSendString> sendItem;
};

struct AsyncRecvString {
	AsyncRecvString(MsgType type, frigg::SharedPtr<EventHub> event_hub,
			int64_t filter_request, int64_t filter_sequence,
			SubmitInfo submit_info);
	
	MsgType type;
	frigg::SharedPtr<EventHub> eventHub;
	SubmitInfo submitInfo;
	int64_t filterRequest;
	int64_t filterSequence;
	uint32_t flags;
	
	// used by kMsgStringToBuffer
	ForeignSpaceLock spaceLock;
	
	// used by kMsgStringToRing
	frigg::SharedPtr<RingBuffer> ringBuffer;
	
	frigg::IntrusiveSharedLinkedItem<AsyncRecvString> recvItem;
};

struct AsyncRingItem : public AsyncOperation {
	AsyncRingItem(AsyncData data, DirectSpaceLock<HelRingBuffer> space_lock,
			size_t buffer_size);

	DirectSpaceLock<HelRingBuffer> spaceLock;
	size_t bufferSize;

	size_t offset;

	frigg::IntrusiveSharedLinkedItem<AsyncRingItem> bufferItem;
};

class RingBuffer {
public:
	RingBuffer();

	void submitBuffer(frigg::SharedPtr<AsyncRingItem> item);

	void doTransfer(frigg::SharedPtr<AsyncSendString> send,
			frigg::SharedPtr<AsyncRecvString> recv);

private:
	frigg::IntrusiveSharedLinkedList<
		AsyncRingItem,
		&AsyncRingItem::bufferItem
	> _bufferQueue;
};

} // namespace thor

