
namespace thor {

// --------------------------------------------------------
// I/O related functions
// --------------------------------------------------------

class IrqRelay {
public:
	enum {
		kFlagExclusive = 1,
		kFlagManualAcknowledge = 2
	};

	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	IrqRelay();

	void addLine(Guard &guard, frigg::WeakPtr<IrqLine> line);

	void setup(Guard &guard, uint32_t flags);

	void fire(Guard &guard);

	void manualAcknowledge(Guard &guard);

	Lock lock;

private:
	uint32_t p_flags;

	uint64_t p_sequence;
	
	frigg::Vector<frigg::WeakPtr<IrqLine>, KernelAlloc> p_lines;
};

extern frigg::LazyInitializer<IrqRelay> irqRelays[16];

struct AwaitIrqBase {
	virtual void complete(Error error) = 0;

	frigg::IntrusiveSharedLinkedItem<AwaitIrqBase> processQueueItem;
};

template<typename F>
struct AwaitIrq : AwaitIrqBase {
	AwaitIrq(F functor)
	: _functor(frigg::move(functor)) { }

	void complete(Error error) override {
		_functor(error);
	}

private:
	F _functor;
};

class IrqLine {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	IrqLine(int number);

	int getNumber();

	void submitWait(Guard &guard, frigg::SharedPtr<AwaitIrqBase> wait);
	
	void fire(Guard &guard, uint64_t sequence);

	Lock lock;

private:
	void processWait(frigg::SharedPtr<AwaitIrqBase> wait);

	int _number;
	
	uint64_t _firedSequence;
	uint64_t _notifiedSequence;
	
	frigg::IntrusiveSharedLinkedList<
		AwaitIrqBase,
		&AwaitIrqBase::processQueueItem
	> _waitQueue;
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

