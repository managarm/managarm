
namespace thor {

// --------------------------------------------------------
// I/O related functions
// --------------------------------------------------------

class IrqRelay {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	IrqRelay();

	void submitWaitRequest(Guard &guard, KernelSharedPtr<EventHub> &&event_hub,
			SubmitInfo submit_info);
	
	void fire(Guard &guard);

	Lock lock;

private:
	struct Request {
		Request(KernelSharedPtr<EventHub> &&event_hub,
				SubmitInfo submit_info);

		KernelSharedPtr<EventHub> eventHub;
		SubmitInfo submitInfo;
	};

	frigg::util::LinkedList<Request, KernelAlloc> p_requests;
};

extern frigg::util::LazyInitializer<IrqRelay> irqRelays[16];

class IrqLine {
public:
	IrqLine(int number);

	int getNumber();

private:
	int p_number;
};

class IoSpace {
public:
	IoSpace();

	void addPort(uintptr_t port);

	void enableInThread(KernelUnsafePtr<Thread> thread);

private:
	frigg::util::Vector<uintptr_t, KernelAlloc> p_ports;
};

} // namespace thor

