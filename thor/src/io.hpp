
namespace thor {

// --------------------------------------------------------
// I/O related functions
// --------------------------------------------------------

class IrqRelay {
public:
	IrqRelay();

	void submitWaitRequest(SharedPtr<EventHub, KernelAlloc> &&event_hub,
			SubmitInfo submit_info);
	
	void fire();

private:
	struct Request {
		Request(SharedPtr<EventHub, KernelAlloc> &&event_hub,
				SubmitInfo submit_info);

		SharedPtr<EventHub, KernelAlloc> eventHub;
		SubmitInfo submitInfo;
	};

	frigg::util::LinkedList<Request, KernelAlloc> p_requests;
};

extern LazyInitializer<IrqRelay> irqRelays[16];

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

	void enableInThread(UnsafePtr<Thread, KernelAlloc> thread);

private:
	frigg::util::Vector<uintptr_t, KernelAlloc> p_ports;
};

} // namespace thor

