#pragma once

#include <frg/list.hpp>
#include <frg/string.hpp>
#include <frigg/debug.hpp>
#include <frigg/linked.hpp>
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
	null,
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

private:
	frg::string<KernelAlloc> _name;

	IrqPin *_pin;
	
	// Must be protected against IRQs.
	frg::ticket_spinlock _mutex;

	// The following fields are protected by pin->_mutex and _mutex.
private:
	uint64_t _currentSequence;
	uint64_t _responseSequence;
	IrqStatus _status;
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
	static constexpr int maskedForNack = 2;

public:
	static void attachSink(IrqPin *pin, IrqSink *sink);
	static Error ackSink(IrqSink *sink, uint64_t sequence);
	static Error nackSink(IrqSink *sink, uint64_t sequence);
	static Error kickSink(IrqSink *sink);

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
	void _kick();

public:
	void warnIfPending();

protected:
	virtual IrqStrategy program(TriggerMode mode, Polarity polarity) = 0;

	virtual void mask() = 0;
	virtual void unmask() = 0;

	// Sends an end-of-interrupt signal to the interrupt controller.
	virtual void sendEoi() = 0;

private:
	void _callSinks();
	void _updateMask();

	frg::string<KernelAlloc> _name;

	// Must be protected against IRQs.
	frg::ticket_spinlock _mutex;

	IrqConfiguration _activeCfg;

	IrqStrategy _strategy;

	uint64_t _raiseSequence;
	uint64_t _sinkSequence;
	bool _inService;
	unsigned int _dueSinks;
	int _maskState;

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
struct IrqObject final : IrqSink {
	IrqObject(frg::string<KernelAlloc> name);

	void automate(frigg::SharedPtr<BoundKernlet> kernlet);

	IrqStatus raise() override;

	void submitAwait(AwaitIrqNode *node, uint64_t sequence);

private:
	frigg::SharedPtr<BoundKernlet> _automationKernlet;

	// Protected by the sinkMutex.
	frg::intrusive_list<
		AwaitIrqNode,
		frg::locate_member<
			AwaitIrqNode,
			frg::default_list_hook<AwaitIrqNode>,
			&AwaitIrqNode::_queueNode
		>
	> _waitQueue;
};

} // namespace thor
