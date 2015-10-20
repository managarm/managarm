
namespace thor {

// --------------------------------------------------------
// I/O related functions
// --------------------------------------------------------

class IrqRelay {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	IrqRelay();

	void addLine(Guard &guard, frigg::WeakPtr<IrqLine> line);

	void fire(Guard &guard);

	Lock lock;

private:
	uint64_t p_sequence;
	
	frigg::Vector<frigg::WeakPtr<IrqLine>, KernelAlloc> p_lines;
};

extern frigg::LazyInitializer<IrqRelay> irqRelays[16];

class IrqLine {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	IrqLine(int number);

	int getNumber();

	void submitWait(Guard &guard, KernelSharedPtr<EventHub> event_hub,
			SubmitInfo submit_info);
	
	void fire(Guard &guard, uint64_t sequence);

	Lock lock;

private:
	struct Request : BaseRequest {
		Request(KernelSharedPtr<EventHub> event_hub, SubmitInfo submit_info);
	};

	void processRequest(Request request);

	int p_number;
	
	uint64_t p_firedSequence;
	uint64_t p_notifiedSequence;
	
	frigg::LinkedList<Request, KernelAlloc> p_requests;
};

class IoSpace {
public:
	IoSpace();

	void addPort(uintptr_t port);

	void enableInThread(KernelUnsafePtr<Thread> thread);

private:
	frigg::Vector<uintptr_t, KernelAlloc> p_ports;
};

} // namespace thor

