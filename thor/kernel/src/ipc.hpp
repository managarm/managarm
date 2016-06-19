
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
	Error sendDescriptor(Guard &guard, frigg::SharedPtr<AsyncSendDescriptor> send);
	
	Error submitRecvString(Guard &guard, frigg::SharedPtr<AsyncRecvString> recv);
	Error submitRecvDescriptor(Guard &guard, frigg::SharedPtr<AsyncRecvDescriptor> recv);
	
	void close(Guard &guard);

	Lock lock;

private:
	bool matchStringRequest(frigg::UnsafePtr<AsyncSendString> send,
			frigg::UnsafePtr<AsyncRecvString> recv);
	bool matchDescriptorRequest(frigg::UnsafePtr<AsyncSendDescriptor> send,
			frigg::UnsafePtr<AsyncRecvDescriptor> recv);

	// returns true if the message + request are consumed
	bool processStringRequest(frigg::SharedPtr<AsyncSendString> send,
			frigg::SharedPtr<AsyncRecvString> recv);
	void processDescriptorRequest(frigg::SharedPtr<AsyncSendDescriptor> send,
			frigg::SharedPtr<AsyncRecvDescriptor> recv);

	frigg::IntrusiveSharedLinkedList<
		AsyncSendString,
		&AsyncSendString::processQueueItem
	> _sendStringQueue;

	frigg::IntrusiveSharedLinkedList<
		AsyncSendDescriptor,
		&AsyncSendDescriptor::processQueueItem
	> _sendDescriptorQueue;

	frigg::IntrusiveSharedLinkedList<
		AsyncRecvString,
		&AsyncRecvString::processQueueItem
	> _recvStringQueue;

	frigg::IntrusiveSharedLinkedList<
		AsyncRecvDescriptor,
		&AsyncRecvDescriptor::processQueueItem
	> _recvDescriptorQueue;

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

