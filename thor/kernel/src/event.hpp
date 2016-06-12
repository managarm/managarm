
namespace thor {

struct UserEvent {
	enum Type {
		kTypeNone,
		kTypeError,
		kTypeMemoryLoad,
		kTypeMemoryLock,
		kTypeJoin,
		// TODO: use only a single kTypeRecvString
		kTypeRecvStringTransferToBuffer,
		kTypeRecvStringTransferToQueue,
		kTypeRecvDescriptor,
		kTypeAccept,
		kTypeConnect,
		kTypeIrq
	};

	UserEvent(Type type, SubmitInfo submit_info);

	Type type;
	SubmitInfo submitInfo;

	// used by kTypeRecvStringError
	Error error;

	// used by kTypeMemoryLoad, kTypeRecvStringTransferToBuffer
	// and kTypeRecvStringTransferToQueue
	size_t offset;
	size_t length;
	
	// used by kTypeRecvStringTransferToBuffer, kTypeRecvStringTransferToQueue
	// and kTypeRecvDescriptor
	int64_t msgRequest;
	int64_t msgSequence;

	// used by kTypeAccept, kTypeConnect
	KernelSharedPtr<Endpoint> endpoint;

	// used by kTypeRecvDescriptor
	AnyDescriptor descriptor;
};

class EventHub {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	EventHub();

	void raiseEvent(Guard &guard, UserEvent &&event);

	bool hasEvent(Guard &guard);

	UserEvent dequeueEvent(Guard &guard);

	void blockCurrentThread(Guard &guard);

	Lock lock;

private:
	frigg::LinkedList<UserEvent, KernelAlloc> p_queue;
	frigg::LinkedList<KernelWeakPtr<Thread>, KernelAlloc> p_waitingThreads;
};

} // namespace thor

