
namespace thor {

// Single producer, single consumer connection
class Channel {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	Channel();

	void sendString(Guard &guard, const void *user_buffer, size_t length,
			int64_t msg_request, int64_t msg_sequence);
	
	void sendDescriptor(Guard &guard, AnyDescriptor &&descriptor,
			int64_t msg_request, int64_t msg_sequence);
	
	void submitRecvString(Guard &guard, KernelSharedPtr<EventHub> &&event_hub,
			void *user_buffer, size_t length,
			int64_t filter_request, int64_t filter_sequence,
			SubmitInfo submit_info);
	
	void submitRecvDescriptor(Guard &guard, KernelSharedPtr<EventHub> &&event_hub,
			int64_t filter_request, int64_t filter_sequence,
			SubmitInfo submit_info);

	Lock lock;

private:
	enum MsgType {
		kMsgNone,
		kMsgString,
		kMsgDescriptor
	};

	struct Message {
		Message(MsgType type, int64_t msg_request, int64_t msg_sequence);
		
		MsgType type;
		frigg::UniqueMemory<KernelAlloc> kernelBuffer;
		size_t length;
		AnyDescriptor descriptor;
		int64_t msgRequest;
		int64_t msgSequence;
	};

	struct Request {
		Request(MsgType type, KernelSharedPtr<EventHub> &&event_hub,
				int64_t filter_request, int64_t filter_sequence,
				SubmitInfo submit_info);
		
		MsgType type;
		KernelSharedPtr<EventHub> eventHub;
		SubmitInfo submitInfo;
		void *userBuffer;
		size_t maxLength;
		int64_t filterRequest;
		int64_t filterSequence;
	};

	bool matchRequest(const Message &message, const Request &request);

	// returns true if the message + request are consumed
	bool processStringRequest(Message &message, Request &request);

	void processDescriptorRequest(Message &message, Request &request);

	frigg::LinkedList<Message, KernelAlloc> p_messages;
	frigg::LinkedList<Request, KernelAlloc> p_requests;
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

