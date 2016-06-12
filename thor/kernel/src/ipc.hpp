
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

	Error sendString(Guard &guard, frigg::SharedPtr<AsyncSendString> send);
	Error sendDescriptor(Guard &guard, frigg::SharedPtr<AsyncSendString> send);
	
	Error submitRecvString(Guard &guard, frigg::SharedPtr<AsyncRecvString> recv);
	Error submitRecvStringToRing(Guard &guard, frigg::SharedPtr<AsyncRecvString> recv);
	Error submitRecvDescriptor(Guard &guard, frigg::SharedPtr<AsyncRecvString> recv);
	
	void close(Guard &guard);

	Lock lock;

private:
	bool matchRequest(frigg::UnsafePtr<AsyncSendString> send,
			frigg::UnsafePtr<AsyncRecvString> recv);

	// returns true if the message + request are consumed
	bool processStringRequest(frigg::SharedPtr<AsyncSendString> send,
			frigg::SharedPtr<AsyncRecvString> recv);
	void processDescriptorRequest(frigg::SharedPtr<AsyncSendString> send,
			frigg::SharedPtr<AsyncRecvString> recv);

	frigg::IntrusiveSharedLinkedList<
		AsyncSendString,
		&AsyncSendString::sendItem
	> _sendQueue;

	frigg::IntrusiveSharedLinkedList<
		AsyncRecvString,
		&AsyncRecvString::recvItem
	> _recvQueue;

	bool _wasClosed;
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

