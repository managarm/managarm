
namespace thor {

// Single producer, single consumer connection
class Channel {
public:
	Channel();

	void sendString(const uint8_t *buffer, size_t length,
			int64_t msg_request, int64_t msg_sequence);
	
	void sendDescriptor(AnyDescriptor &&descriptor,
			int64_t msg_request, int64_t msg_sequence);
	
	void submitRecvString(SharedPtr<EventHub, KernelAlloc> &&event_hub,
			uint8_t *user_buffer, size_t length,
			int64_t filter_request, int64_t filter_sequence,
			SubmitInfo submit_info);
	
	void submitRecvDescriptor(SharedPtr<EventHub, KernelAlloc> &&event_hub,
			int64_t filter_request, int64_t filter_sequence,
			SubmitInfo submit_info);

private:
	enum MsgType {
		kMsgNone,
		kMsgString,
		kMsgDescriptor
	};

	struct Message {
		Message(MsgType type, int64_t msg_request, int64_t msg_sequence);
		
		MsgType type;
		uint8_t *kernelBuffer;
		size_t length;
		AnyDescriptor descriptor;
		int64_t msgRequest;
		int64_t msgSequence;
	};

	struct Request {
		Request(MsgType type, SharedPtr<EventHub, KernelAlloc> &&event_hub,
				int64_t filter_request, int64_t filter_sequence,
				SubmitInfo submit_info);
		
		MsgType type;
		SharedPtr<EventHub, KernelAlloc> eventHub;
		SubmitInfo submitInfo;
		uint8_t *userBuffer;
		size_t maxLength;
		int64_t filterRequest;
		int64_t filterSequence;
	};

	bool matchRequest(const Message &message, const Request &request);

	// returns true if the message + request are consumed
	bool processStringRequest(Message &message, Request &request);

	void processDescriptorRequest(Message &message, Request &request);

	frigg::util::LinkedList<Message, KernelAlloc> p_messages;
	frigg::util::LinkedList<Request, KernelAlloc> p_requests;
};

class BiDirectionPipe {
public:
	BiDirectionPipe();

	Channel *getFirstChannel();
	Channel *getSecondChannel();

private:
	Channel p_firstChannel;
	Channel p_secondChannel;
};

class Server {
public:
	Server();

	void submitAccept(SharedPtr<EventHub, KernelAlloc> &&event_hub,
			SubmitInfo submit_info);
	
	void submitConnect(SharedPtr<EventHub, KernelAlloc> &&event_hub,
			SubmitInfo submit_info);

private:
	struct AcceptRequest {
		AcceptRequest(SharedPtr<EventHub, KernelAlloc> &&event_hub,
				SubmitInfo submit_info);

		SharedPtr<EventHub, KernelAlloc> eventHub;
		SubmitInfo submitInfo;
	};
	struct ConnectRequest {
		ConnectRequest(SharedPtr<EventHub, KernelAlloc> &&event_hub,
				SubmitInfo submit_info);

		SharedPtr<EventHub, KernelAlloc> eventHub;
		SubmitInfo submitInfo;
	};

	void processRequests(const AcceptRequest &accept,
			const ConnectRequest &connect);
	
	frigg::util::LinkedList<AcceptRequest, KernelAlloc> p_acceptRequests;
	frigg::util::LinkedList<ConnectRequest, KernelAlloc> p_connectRequests;
};

} // namespace thor

