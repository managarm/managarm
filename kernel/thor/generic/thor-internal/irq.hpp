#pragma once

#include <frg/expected.hpp>
#include <frg/list.hpp>
#include <frg/string.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/kernlet.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

struct AwaitIrqNode {
	friend struct IrqObject;

	void setup(Worklet *awaited) {
		_awaited = awaited;
	}

	Error error() { return _error; }
	uint64_t sequence() { return _sequence; }

private:
	Worklet *_awaited;

	Error _error;
	uint64_t _sequence;

	frg::default_list_hook<AwaitIrqNode> _queueNode;
};

// ----------------------------------------------------------------------------

struct IrqPin;

// Represents a slot in the CPU's interrupt table.
// Slots might be global or per-CPU.
struct IrqSlot {
	bool isAvailable() {
		return _pin == nullptr;
	}

	// Links an IrqPin to this slot.
	// From now on all IRQ raises will go to this IrqPin.
	void link(IrqPin *pin);

	// The kernel calls this function when an IRQ is raised.
	void raise();

	IrqPin *pin() {
		return _pin;
	}

private:
	IrqPin *_pin = nullptr;
};

// ----------------------------------------------------------------------------

enum class TriggerMode {
	null,
	edge,
	level
};

enum class Polarity {
	null,
	high,
	low
};

struct IrqConfiguration {
	bool specified() {
		return trigger != TriggerMode::null
				&& polarity != Polarity::null;
	}

	bool compatible(IrqConfiguration other) {
		assert(specified());
		return trigger == other.trigger
				&& polarity == other.polarity;
	}

	TriggerMode trigger = TriggerMode::null;
	Polarity polarity = Polarity::null;
};

// ----------------------------------------------------------------------------

enum class IrqStatus {
	standBy,
	indefinite,
	acked,
	nacked
};

struct IrqSink {
	friend struct IrqPin;

	IrqSink(frg::string<KernelAlloc> name);

	const frg::string<KernelAlloc> &name() {
		return _name;
	}

	// This method is called with sinkMutex() held.
	virtual IrqStatus raise() = 0;

	virtual void dumpHardwareState();

	// TODO: This needs to be thread-safe.
	IrqPin *getPin();

	frg::default_list_hook<IrqSink> hook;

protected:
	frg::ticket_spinlock *sinkMutex() {
		return &_mutex;
	}

	// Protected by the pin->_mutex and sinkMutex().
	uint64_t currentSequence() {
		return _currentSequence;
	}

	~IrqSink() = default;

private:
	frg::string<KernelAlloc> _name;

	IrqPin *_pin;
	
	// Must be protected against IRQs.
	frg::ticket_spinlock _mutex;

	// The following fields are protected by pin->_mutex and _mutex.
private:
	uint64_t _currentSequence;
	IrqStatus _status = IrqStatus::standBy;
};

enum class IrqStrategy {
	null,
	justEoi,
	maskThenEoi
};

// Represents a (not necessarily physical) "pin" of an interrupt controller.
// This class handles the IRQ configuration and acknowledgement.
struct IrqPin {
private:
	static constexpr int maskedForService = 1;
	static constexpr int maskedWhileBuffered = 2;
	static constexpr int maskedForNack = 4;

public:
	static void attachSink(IrqPin *pin, IrqSink *sink);
	static Error ackSink(IrqSink *sink, uint64_t sequence);
	static Error nackSink(IrqSink *sink, uint64_t sequence);
	static Error kickSink(IrqSink *sink, bool wantClear);

public:
	IrqPin(frg::string<KernelAlloc> name);

	IrqPin(const IrqPin &) = delete;
	
	IrqPin &operator= (const IrqPin &) = delete;

	const frg::string<KernelAlloc> &name() {
		return _name;
	}

	void configure(IrqConfiguration cfg);

	// This function is called from IrqSlot::raise().
	void raise();

private:
	void _acknowledge();
	void _nack();
	void _kick(bool doClear);
	void _dispatch();

public:
	void warnIfPending();

	virtual void dumpHardwareState();

protected:
	virtual IrqStrategy program(TriggerMode mode, Polarity polarity) = 0;

	virtual void mask() = 0;
	virtual void unmask() = 0;

	// Sends an end-of-interrupt signal to the interrupt controller.
	virtual void sendEoi() = 0;

	~IrqPin() = default;

private:
	void _doService();
	void _updateMask();

	frg::string<KernelAlloc> _name;

	// Must be protected against IRQs.
	frg::ticket_spinlock _mutex;

	IrqConfiguration _activeCfg;

	IrqStrategy _strategy;

	bool _inService;
	// Whether we should immediately re-raise an IRQ if it goes out of service.
	// This is used by edge triggered IRQs to "buffer" (at most one) edge.
	bool _raiseBuffered = false;
	// _dispatchAcks and _dispatchKicks determine how _dispatch() clears the IRQ.
	bool _dispatchAcks = false;
	bool _dispatchKicks = false;
	unsigned int _dueSinks;
	int _maskState;
	unsigned int _maskedRaiseCtr = 0;

	// Timestamp of the last acknowledge() operation.
	// Relative to currentNanos().
	uint64_t _raiseClock;
	
	bool _warnedAfterPending;

	// TODO: This list should change rarely. Use a RCU list.
	frg::intrusive_list<
		IrqSink,
		frg::locate_member<
			IrqSink,
			frg::default_list_hook<IrqSink>,
			&IrqSink::hook
		>
	> _sinkList;
};

// ----------------------------------------------------------------------------

// This class implements the user-visible part of IRQ handling.
struct IrqObject : IrqSink {
	IrqObject(frg::string<KernelAlloc> name);

	void automate(smarter::shared_ptr<BoundKernlet> kernlet);

	IrqStatus raise() override;

	void submitAwait(AwaitIrqNode *node, uint64_t sequence);

	// ----------------------------------------------------------------------------------
	// awaitIrq() and its boilerplate.
	// ----------------------------------------------------------------------------------

	template<typename Receiver>
	struct AwaitIrqOperation : AwaitIrqNode {
		AwaitIrqOperation(IrqObject *object, uint64_t sequence, WorkQueue *wq, Receiver r)
		: object_{object}, sequence_{sequence}, wq_{wq}, r_{std::move(r)} { }

		void start() {
			worklet_.setup([] (Worklet *base) {
				auto self = frg::container_of(base, &AwaitIrqOperation::worklet_);
				if(self->error() != Error::success) {
					async::execution::set_value(self->r_, self->error());
				}else{
					async::execution::set_value(self->r_, self->sequence());
				}
			}, wq_);
			setup(&worklet_);
			object_->submitAwait(this, sequence_);
		}

	private:
		IrqObject *object_;
		uint64_t sequence_;
		WorkQueue *wq_;
		Receiver r_;
		Worklet worklet_;
	};

	struct AwaitIrqSender {
		using value_type = frg::expected<Error, uint64_t>;

		template<typename Receiver>
		friend AwaitIrqOperation<Receiver> connect(AwaitIrqSender s, Receiver r) {
			return {s.object, s.sequence, s.wq, std::move(r)};
		}

		friend async::sender_awaiter<AwaitIrqSender, frg::expected<Error, uint64_t>>
		operator co_await (AwaitIrqSender s) {
			return {s};
		}

		IrqObject *object;
		uint64_t sequence;
		WorkQueue *wq;
	};

	AwaitIrqSender awaitIrq(uint64_t sequence, WorkQueue *wq) {
		return {this, sequence, wq};
	}

private:
	smarter::shared_ptr<BoundKernlet> _automationKernlet;

	// Protected by the sinkMutex.
	frg::intrusive_list<
		AwaitIrqNode,
		frg::locate_member<
			AwaitIrqNode,
			frg::default_list_hook<AwaitIrqNode>,
			&AwaitIrqNode::_queueNode
		>
	> _waitQueue;

protected:
	~IrqObject() = default;
};

struct GenericIrqObject final : IrqObject {
	GenericIrqObject(frg::string<KernelAlloc> name)
	: IrqObject{name} { }
};

} // namespace thor
