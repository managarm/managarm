
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

	void submitAccept(Guard &guard, frigg::SharedPtr<AsyncAccept> request);
	void submitConnect(Guard &guard, frigg::SharedPtr<AsyncConnect> request);
	
	Lock lock;
	
private:
	void processRequests(frigg::SharedPtr<AsyncAccept> accept,
			frigg::SharedPtr<AsyncConnect> connect);
	
	frigg::IntrusiveSharedLinkedList<
		AsyncAccept,
		&AsyncAccept::processItem
	> _acceptQueue;
	
	frigg::IntrusiveSharedLinkedList<
		AsyncConnect,
		&AsyncConnect::processItem
	> _connectQueue;
};

} // namespace thor

