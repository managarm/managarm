#ifndef THOR_GENERIC_IRQ_HPP
#define THOR_GENERIC_IRQ_HPP

#include <frigg/debug.hpp>
#include <frigg/linked.hpp>
#include <frigg/string.hpp>
#include <frg/list.hpp>
#include "error.hpp"
#include "kernel_heap.hpp"

namespace thor {

struct AwaitIrqNode {
	virtual void onRaise(Error error) = 0;

	frigg::IntrusiveSharedLinkedItem<AwaitIrqNode> processQueueItem;
};

// ----------------------------------------------------------------------------

struct IrqPin;

// Represents a slot in the CPU's interrupt table.
// Slots might be global or per-CPU.
struct IrqSlot {
	// Links an IrqPin to this slot.
	// From now on all IRQ raises will go to this IrqPin.
	void link(IrqPin *pin);

	// The kernel calls this function when an IRQ is raised.
	void raise();

private:
	IrqPin *_pin;
};

// ----------------------------------------------------------------------------

using IrqStatus = unsigned int;

namespace irq_status {
	static constexpr IrqStatus null = 0;

	// The IRQ has been handled by the sink's raise() method.
	static constexpr IrqStatus handled = 1;
}

struct IrqSink {
	friend void attachIrq(IrqPin *pin, IrqSink *sink);

	IrqSink();

	virtual IrqStatus raise() = 0;

	// TODO: This needs to be thread-safe.
	IrqPin *getPin();

	frg::default_list_hook<IrqSink> hook;

private:
	IrqPin *_pin;
};

enum class IrqStrategy {
	null,
	justEoi,
	maskThenEoi
};

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

// Represents a (not necessarily physical) "pin" of an interrupt controller.
// This class handles the IRQ configuration and acknowledgement.
struct IrqPin {
	friend void attachIrq(IrqPin *pin, IrqSink *sink);
private:
	using Mutex = frigg::TicketLock;

public:
	IrqPin(frigg::String<KernelAlloc> name);

	IrqPin(const IrqPin &) = delete;
	
	IrqPin &operator= (const IrqPin &) = delete;

	const frigg::String<KernelAlloc> &name() {
		return _name;
	}

	void configure(TriggerMode mode, Polarity polarity);

	// This function is called from IrqSlot::raise().
	void raise();

	void kick();

	void acknowledge();

	void warnIfPending();

protected:
	virtual IrqStrategy program(TriggerMode mode, Polarity polarity) = 0;

	virtual void mask() = 0;
	virtual void unmask() = 0;

	// Sends an end-of-interrupt signal to the interrupt controller.
	virtual void sendEoi() = 0;

private:
	IrqStatus _callSinks();

	frigg::String<KernelAlloc> _name;

	// Must be protected against IRQs.
	Mutex _mutex;

	IrqStrategy _strategy;
	bool _latched;
	bool _warnedAfterPending;

	// Timestamp of the last acknowledge() operation.
	// Relative to currentNanos().
	uint64_t _raiseClock;

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

void attachIrq(IrqPin *pin, IrqSink *sink);

// ----------------------------------------------------------------------------

// This class implements the user-visible part of IRQ handling.
struct IrqObject : IrqSink {
private:
	using Mutex = frigg::TicketLock;

public:
	IrqObject();

	IrqStatus raise() override;

	void submitAwait(frigg::SharedPtr<AwaitIrqNode> wait);
	
	void acknowledge();
	
private:
	// Must be protected against IRQs.
	Mutex _mutex;

	// Protected by the mutex.
	bool _latched;

	// Protected by the mutex.
	frigg::IntrusiveSharedLinkedList<
		AwaitIrqNode,
		&AwaitIrqNode::processQueueItem
	> _waitQueue;
};

} // namespace thor

#endif // THOR_GENERIC_IRQ_HPP
