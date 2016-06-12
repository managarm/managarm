
namespace thor {

struct AsyncOperation {
	AsyncOperation(frigg::WeakPtr<EventHub> event_hub, SubmitInfo submit_info);
	
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

struct AsyncSendString {
	AsyncSendString(MsgType type, int64_t msg_request, int64_t msg_sequence);
	
	MsgType type;
	frigg::UniqueMemory<KernelAlloc> kernelBuffer;
	AnyDescriptor descriptor;
	int64_t msgRequest;
	int64_t msgSequence;
	uint32_t flags;
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
};

struct AsyncRingItem : public AsyncOperation {
	AsyncRingItem(frigg::WeakPtr<EventHub> event_hub, SubmitInfo submit_info,
			DirectSpaceLock<HelRingBuffer> space_lock, size_t buffer_size);

	DirectSpaceLock<HelRingBuffer> spaceLock;
	size_t bufferSize;

	size_t offset;
};

class RingBuffer {
public:
	RingBuffer();

	void submitBuffer(AsyncRingItem item);

	void doTransfer(AsyncSendString send, AsyncRecvString recv);

private:
	frigg::LinkedList<AsyncRingItem, KernelAlloc> _items;
};

} // namespace thor

