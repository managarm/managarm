
namespace thor {

// Single producer, single consumer connection
class Channel {
public:
	enum {
		kFlagRequest = 1,
		kFlagResponse = 2
	};

	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	Channel();

	Error sendString(Guard &guard, const void *user_buffer, size_t length,
			int64_t msg_request, int64_t msg_sequence, uint32_t flags);
	
	Error sendDescriptor(Guard &guard, AnyDescriptor &&descriptor,
			int64_t msg_request, int64_t msg_sequence, uint32_t flags);
	
	Error submitRecvString(Guard &guard, frigg::SharedPtr<EventHub> event_hub,
			ForeignSpaceLock space_lock,
			int64_t filter_request, int64_t filter_sequence,
			SubmitInfo submit_info, uint32_t flags);
	Error submitRecvStringToRing(Guard &guard, frigg::SharedPtr<EventHub> event_hub,
			frigg::SharedPtr<RingBuffer> ring_buffer,
			int64_t filter_request, int64_t filter_sequence,
			SubmitInfo submit_info, uint32_t flags);
	
	Error submitRecvDescriptor(Guard &guard, frigg::SharedPtr<EventHub> event_hub,
			int64_t filter_request, int64_t filter_sequence,
			SubmitInfo submit_info, uint32_t flags);
	
	void close(Guard &guard);

	Lock lock;

private:
	bool matchRequest(const AsyncSendString &message, const AsyncRecvString &request);

	// returns true if the message + request are consumed
	bool processStringRequest(AsyncSendString &message, AsyncRecvString &request);

	void processDescriptorRequest(AsyncSendString &message, AsyncRecvString &request);

	frigg::LinkedList<AsyncSendString, KernelAlloc> p_messages;
	frigg::LinkedList<AsyncRecvString, KernelAlloc> p_requests;
	bool p_wasClosed;
};

class Endpoint;

class FullPipe {
public:
	static void create(KernelSharedPtr<FullPipe> &pipe,
			KernelSharedPtr<Endpoint> &end1, KernelSharedPtr<Endpoint> &end2);

	Channel &getChannel(size_t index);

private:
	Channel p_channels[2];
};

class Endpoint {
public:
	Endpoint(KernelSharedPtr<FullPipe> pipe, size_t read_index, size_t write_index);
	~Endpoint();

	KernelUnsafePtr<FullPipe> getPipe();

	size_t getReadIndex();
	size_t getWriteIndex();

private:
	KernelSharedPtr<FullPipe> p_pipe;
	size_t p_readIndex;
	size_t p_writeIndex;
};

class Server {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	Server();

	void submitAccept(Guard &guard, KernelSharedPtr<EventHub> &&event_hub,
			SubmitInfo submit_info);
	
	void submitConnect(Guard &guard, KernelSharedPtr<EventHub> &&event_hub,
			SubmitInfo submit_info);
	
	Lock lock;
	
private:
	struct AcceptRequest {
		AcceptRequest(KernelSharedPtr<EventHub> &&event_hub,
				SubmitInfo submit_info);

		KernelSharedPtr<EventHub> eventHub;
		SubmitInfo submitInfo;
	};
	struct ConnectRequest {
		ConnectRequest(KernelSharedPtr<EventHub> &&event_hub,
				SubmitInfo submit_info);

		KernelSharedPtr<EventHub> eventHub;
		SubmitInfo submitInfo;
	};

	void processRequests(const AcceptRequest &accept,
			const ConnectRequest &connect);
	
	frigg::LinkedList<AcceptRequest, KernelAlloc> p_acceptRequests;
	frigg::LinkedList<ConnectRequest, KernelAlloc> p_connectRequests;
};

} // namespace thor

