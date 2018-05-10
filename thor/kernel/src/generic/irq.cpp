
#include "irq.hpp"
#include "../arch/x86/ints.hpp"
#include "../arch/x86/hpet.hpp"

namespace thor {

// --------------------------------------------------------
// IrqSlot
// --------------------------------------------------------

void IrqSlot::raise() {
	assert(_pin);
	_pin->raise();
}

void IrqSlot::link(IrqPin *pin) {
	assert(!_pin);
	_pin = pin;
}

// --------------------------------------------------------
// IrqPin
// --------------------------------------------------------

IrqSink::IrqSink()
: _pin{nullptr} { }

IrqPin *IrqSink::getPin() {
	return _pin;
}

IrqPin::IrqPin(frigg::String<KernelAlloc> name)
: _name{std::move(name)}, _strategy{IrqStrategy::null},
		_raiseSequence{0}, _sinkSequence{0}, _wasAcked{true} { }

void IrqPin::configure(TriggerMode mode, Polarity polarity) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	frigg::infoLogger() << "thor: Configuring IRQ " << _name
			<< " to trigger mode: " << static_cast<int>(mode)
			<< ", polarity: " << static_cast<int>(polarity) << frigg::endLog;

	_strategy = program(mode, polarity);
	_raiseSequence = 0;
	_sinkSequence = 0;
	_wasAcked = true;
}

void IrqPin::raise() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	
	if(_strategy == IrqStrategy::null) {
		frigg::infoLogger() << "\e[35mthor: Unconfigured IRQ was raised\e[39m" << frigg::endLog;
	}else{
		assert(_strategy == IrqStrategy::justEoi
				|| _strategy == IrqStrategy::maskThenEoi);
	}

	auto active = !_wasAcked;
	_raiseSequence++;
	_wasAcked = false;

	if(active) {
		// TODO: This is not an error, just a hardware race. Report it less obnoxiously.
		// TODO: If the IRQ is edge-triggered we lose an edge here!
		frigg::infoLogger() << "\e[35mthor: Ignoring already active IRQ " << _name
				<< "\e[39m" << frigg::endLog;
	}else{
		_sinkSequence = _raiseSequence;
		auto status = _callSinks();
		if(status & irq_status::handled) {
			_wasAcked = true;
		}else{
			_raiseClock = currentNanos();
			_warnedAfterPending = false;
		}
	}
	
	if(!_wasAcked && _strategy == IrqStrategy::maskThenEoi)
		mask();

	sendEoi();
}

void IrqPin::kick() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	if(_wasAcked)
		return;
	_wasAcked = true;

	// Avoid losing IRQs that were ignored in raise() as 'already active'.
	if(_sinkSequence < _raiseSequence) {
		_sinkSequence = _raiseSequence;
		_callSinks();
	}

	if(_strategy == IrqStrategy::maskThenEoi)
		unmask();
}

void IrqPin::acknowledge() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	// Commit ef3927ac1f introduced a check against the input sequence number here.
	// It is not clear what the purpose of this check is; certainly, it can lead to
	// missing ACKs for shared IRQs.
//	assert(sequence <= _currentSequence);
//	if(sequence < _currentSequence || _wasAcked)
//		return;

	if(_wasAcked)
		return;
	_wasAcked = true;

	// Avoid losing IRQs that were ignored in raise() as 'already active'.
	if(_sinkSequence < _raiseSequence) {
		_sinkSequence = _raiseSequence;
		_callSinks();
	}

	if(_strategy == IrqStrategy::maskThenEoi)
		unmask();
}

void IrqPin::warnIfPending() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	if(!_wasAcked && currentNanos() - _raiseClock > 1000000000 && !_warnedAfterPending) {
		frigg::infoLogger() << "\e[35mthor: Pending IRQ " << _name << " has not been acked"
				" for more than one second.\e[39m" << frigg::endLog;
		_warnedAfterPending = true;
	}
}

IrqStatus IrqPin::_callSinks() {
	if(_sinkList.empty())
		frigg::infoLogger() << "\e[35mthor: No sink for IRQ "
				<< _name << "\e[39m" << frigg::endLog;

	auto status = irq_status::null;
	for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it)
		status |= (*it)->raise(_sinkSequence);
	return status;
}

void attachIrq(IrqPin *pin, IrqSink *sink) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&pin->_mutex);

	assert(!sink->_pin);
	pin->_sinkList.push_back(sink);
	sink->_pin = pin;
}

// --------------------------------------------------------
// IrqObject
// --------------------------------------------------------

// We create the IrqObject in latched state in order to ensure that users to not miss IRQs
// that happened before the object was created.
// However this can result in spurious raises.
IrqObject::IrqObject()
: _currentSequence{0} { }

IrqStatus IrqObject::raise(uint64_t sequence) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(sequence > _currentSequence);
	_currentSequence = sequence;

	while(!_waitQueue.empty()) {
		auto node = _waitQueue.pop_front();
		node->onRaise(kErrSuccess, _currentSequence);
	}

	return irq_status::null;
}

void IrqObject::submitAwait(AwaitIrqNode *node, uint64_t sequence) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(sequence <= _currentSequence);
	if(sequence < _currentSequence) {
		node->onRaise(kErrSuccess, _currentSequence);
	}else{
		_waitQueue.push_back(node);
	}
}

void IrqObject::kick() {
	getPin()->kick();
}

void IrqObject::acknowledge() {
	getPin()->acknowledge();
}

} // namespace thor

