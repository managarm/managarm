
namespace thor {

// Single producer, single consumer connection
class Channel {
public:
	struct Message {
	public:
		Message(uint8_t *kernel_buffer, size_t length,
			int64_t msg_request, int64_t msg_sequence);

		uint8_t *kernelBuffer;
		size_t length;
		int64_t msgRequest;
		int64_t msgSequence;
	};

	Channel();

	void sendString(const uint8_t *buffer, size_t length,
			int64_t msg_request, int64_t msg_sequence);
	void submitRecvString(SharedPtr<EventHub> &&event_hub,
			uint8_t *user_buffer, size_t length,
			int64_t filter_request, int64_t filter_sequence,
			SubmitInfo submit_info);

private:
	struct Request {
		Request(SharedPtr<EventHub> &&event_hub,
				int64_t filter_request, int64_t filter_sequence,
				SubmitInfo submit_info);

		SharedPtr<EventHub> eventHub;
		SubmitInfo submitInfo;
		uint8_t *userBuffer;
		size_t maxLength;
		int64_t filterRequest;
		int64_t filterSequence;
	};

	bool matchRequest(const Message &message, const Request &request);

	// returns true if the message + request are consumed
	bool processStringRequest(const Message &message, const Request &request);

	util::LinkedList<Message, KernelAlloc> p_messages;
	util::LinkedList<Request, KernelAlloc> p_requests;
};

class BiDirectionPipe : public SharedBase<BiDirectionPipe> {
public:
	BiDirectionPipe();

	Channel *getFirstChannel();
	Channel *getSecondChannel();

private:
	Channel p_firstChannel;
	Channel p_secondChannel;
};

// Reads from the first channel, writes to the second
class BiDirectionFirstDescriptor {
public:
	BiDirectionFirstDescriptor(SharedPtr<BiDirectionPipe> &&pipe);
	
	UnsafePtr<BiDirectionPipe> getPipe();

private:
	SharedPtr<BiDirectionPipe> p_pipe;
};

// Reads from the second channel, writes to the first
class BiDirectionSecondDescriptor {
public:
	BiDirectionSecondDescriptor(SharedPtr<BiDirectionPipe> &&pipe);
	
	UnsafePtr<BiDirectionPipe> getPipe();

private:
	SharedPtr<BiDirectionPipe> p_pipe;
};

class Server : public SharedBase<Server> {
public:
	Server();

	void submitAccept(SharedPtr<EventHub> &&event_hub,
			SubmitInfo submit_info);
	
	void submitConnect(SharedPtr<EventHub> &&event_hub,
			SubmitInfo submit_info);

private:
	struct AcceptRequest {
		AcceptRequest(SharedPtr<EventHub> &&event_hub,
				SubmitInfo submit_info);

		SharedPtr<EventHub> eventHub;
		SubmitInfo submitInfo;
	};
	struct ConnectRequest {
		ConnectRequest(SharedPtr<EventHub> &&event_hub,
				SubmitInfo submit_info);

		SharedPtr<EventHub> eventHub;
		SubmitInfo submitInfo;
	};

	void processRequests(const AcceptRequest &accept,
			const ConnectRequest &connect);
	
	util::LinkedList<AcceptRequest, KernelAlloc> p_acceptRequests;
	util::LinkedList<ConnectRequest, KernelAlloc> p_connectRequests;
};

class ServerDescriptor {
public:
	ServerDescriptor(SharedPtr<Server> &&server);
	
	UnsafePtr<Server> getServer();

private:
	SharedPtr<Server> p_server;
};

class ClientDescriptor {
public:
	ClientDescriptor(SharedPtr<Server> &&server);
	
	UnsafePtr<Server> getServer();

private:
	SharedPtr<Server> p_server;
};

} // namespace thor

