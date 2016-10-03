
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
	~Channel();

	Error sendString(Guard &guard, frigg::SharedPtr<AsyncSendString> send);
	Error sendDescriptor(Guard &guard, frigg::SharedPtr<AsyncSendDescriptor> send);
	
	Error submitRecvString(Guard &guard, frigg::SharedPtr<AsyncRecvString> recv);
	Error submitRecvDescriptor(Guard &guard, frigg::SharedPtr<AsyncRecvDescriptor> recv);
	
	void closeReadEndpoint(Guard &guard);
	void closeWriteEndpoint(Guard &guard);

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

	bool _readEndpointClosed;
	bool _writeEndpointClosed;
};

struct Endpoint {
	friend class EndpointRwControl;

	static frigg::SharedPtr<Channel> readChannel(frigg::SharedPtr<Endpoint> endpoint) {
		return frigg::SharedPtr<Channel>(frigg::move(endpoint), endpoint->_read);
	}
	static frigg::SharedPtr<Channel> writeChannel(frigg::SharedPtr<Endpoint> endpoint) {
		return frigg::SharedPtr<Channel>(frigg::move(endpoint), endpoint->_write);
	}

	Endpoint(Channel *read, Channel *write)
	: _read(read), _write(write), _rwCount(1) { }

private:
	Channel *_read;
	Channel *_write;
	int _rwCount;
};

class FullPipe {
public:
	FullPipe()
	: _endpoints{ { &_channels[0], &_channels[1] }, { &_channels[1], &_channels[0] } } { }

	Endpoint &endpoint(int index) {
		return _endpoints[index];
	}

private:
	Channel _channels[2];
	Endpoint _endpoints[2];
};

} // namespace thor

