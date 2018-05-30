
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
// IrqSink
// --------------------------------------------------------

IrqSink::IrqSink(frigg::String<KernelAlloc> name)
: _name{frigg::move(name)}, _pin{nullptr}, _currentSequence{0}, _status{IrqStatus::null} { }

IrqPin *IrqSink::getPin() {
	return _pin;
}

// --------------------------------------------------------
// IRQ management functions.
// --------------------------------------------------------

void IrqPin::attachSink(IrqPin *pin, IrqSink *sink) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&pin->_mutex);

	assert(!sink->_pin);
	pin->_sinkList.push_back(sink);
	sink->_pin = pin;
}

Error IrqPin::ackSink(IrqSink *sink) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&pin->_mutex);
	
	if(sink->currentSequence() != pin->_sinkSequence)
		return kErrSuccess;

	// TODO: We would like to prevent users from acking and nacking the same IRQ.
	// Unfortunately, we cannot simply set _status as that is reset at every
	// _sinkSequence but not at every _raiseSequence.
	// TODO: Design a mechanism to detect multiple acks.

	pin->_acknowledge();
	return kErrSuccess;
}

Error IrqPin::nackSink(IrqSink *sink, uint64_t sequence) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&pin->_mutex);
	
	assert(sequence <= sink->currentSequence());
	if(sink->currentSequence() != pin->_sinkSequence)
		return kErrSuccess;
	if(sequence != sink->currentSequence())
		return kErrSuccess;

	if(sink->_status != IrqStatus::null)
		return kErrIllegalArgs;
	sink->_status = IrqStatus::nacked;

	pin->_nack();

	return kErrSuccess;
}

Error IrqPin::kickSink(IrqSink *sink) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&pin->_mutex);
	
	pin->_kick();
	return kErrSuccess;
}

// --------------------------------------------------------
// IrqPin
// --------------------------------------------------------

IrqPin::IrqPin(frigg::String<KernelAlloc> name)
: _name{std::move(name)}, _strategy{IrqStrategy::null},
		_raiseSequence{0}, _sinkSequence{0}, _inService{false}, _dueSinks{0},
		_maskState{0} { }

void IrqPin::configure(TriggerMode mode, Polarity polarity) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	frigg::infoLogger() << "thor: Configuring IRQ " << _name
			<< " to trigger mode: " << static_cast<int>(mode)
			<< ", polarity: " << static_cast<int>(polarity) << frigg::endLog;

	_strategy = program(mode, polarity);
	_raiseSequence = 0;
	_sinkSequence = 0;
	_inService = false;
	_dueSinks = 0;
	_maskState = 0;
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

	auto already_in_service = _inService;
	_raiseSequence++;
	_inService = true;
	
	if(already_in_service) {
		// TODO: This is not an error, just a hardware race. Report it less obnoxiously.
		// TODO: If the IRQ is edge-triggered we lose an edge here!
		//frigg::infoLogger() << "\e[35mthor: Ignoring already active IRQ " << _name
		//		<< "\e[39m" << frigg::endLog;
		_maskState |= maskedForService;
	}else{
		_callSinks();

		if(_inService && !_dueSinks) {
			frigg::infoLogger() << "\e[31mthor: IRQ " << _name << " was nacked!" << frigg::endLog;
			_maskState |= maskedForNack;
		}
	}

	if(_strategy == IrqStrategy::maskThenEoi)
		if(_inService)
			_maskState |= maskedForService;

	_updateMask();
	sendEoi();
}

void IrqPin::_acknowledge() {
	// Commit ef3927ac1f introduced a check against the input sequence number here.
	// It is not clear what the purpose of this check is; certainly, it can lead to
	// missing ACKs for shared IRQs.
//	assert(sequence <= _currentSequence);
//	if(sequence < _currentSequence || _wasAcked)
//		return;

	if(!_inService)
		return;
	_inService = false;

	// Avoid losing IRQs that were ignored in raise() as 'already active'.
	if(_sinkSequence < _raiseSequence)
		_callSinks();

	_maskState &= ~maskedForService;
	_updateMask();
}

void IrqPin::_nack() {
	assert(_dueSinks);
	_dueSinks--;
	
	if(!_inService || _dueSinks)
		return;

	frigg::infoLogger() << "\e[31mthor: IRQ " << _name << " was nacked!" << frigg::endLog;
	_maskState |= maskedForNack;
	_updateMask();
}

void IrqPin::_kick() {
	if(!_inService)
		return;
	_inService = false;

	// Avoid losing IRQs that were ignored in raise() as 'already active'.
	if(_sinkSequence < _raiseSequence)
		_callSinks();

	_maskState &= ~(maskedForService | maskedForNack);
	unmask();
}

void IrqPin::warnIfPending() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	if(!_inService || !_dueSinks)
		return;

	if(currentNanos() - _raiseClock > 1000000000 && !_warnedAfterPending) {
		auto log = frigg::infoLogger();
		log << "\e[35mthor: Pending IRQ " << _name << " has not been"
				" acked/nacked for more than one second.";
		for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it)
			if((*it)->_status == IrqStatus::null)
				log << "\n   Sink " << (*it)->name() << " has not acked/nacked";
		log << "\e[39m" << frigg::endLog;
		_warnedAfterPending = true;
	}
}

void IrqPin::_callSinks() {
	assert(_raiseSequence > _sinkSequence);
	_sinkSequence = _raiseSequence;
	_dueSinks = 0;

	if(_inService) {
		_raiseClock = currentNanos();
		_warnedAfterPending = false;
	}

	if(_sinkList.empty())
		frigg::infoLogger() << "\e[35mthor: No sink for IRQ "
				<< _name << "\e[39m" << frigg::endLog;

	for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it) {
		(*it)->_currentSequence = _sinkSequence;
		(*it)->_status = (*it)->raise();

		if((*it)->_status == IrqStatus::acked) {
			_inService = false;
		}else if((*it)->_status == IrqStatus::nacked) {
			// We do not need to do anything here.
		}else{
			_dueSinks++;
		}
	}
}

void IrqPin::_updateMask() {
	// TODO: Avoid the virtual calls if the state does not change?
	if(!_maskState) {
		unmask();
	}else if(_maskState) {
		mask();
	}
}

// --------------------------------------------------------
// IrqObject
// --------------------------------------------------------

// We create the IrqObject in latched state in order to ensure that users to not miss IRQs
// that happened before the object was created.
// However this can result in spurious raises.
IrqObject::IrqObject(frigg::String<KernelAlloc> name)
: IrqSink{frigg::move(name)} { }

IrqStatus IrqObject::raise() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	while(!_waitQueue.empty()) {
		auto node = _waitQueue.pop_front();
		node->onRaise(kErrSuccess, currentSequence());
	}

	return IrqStatus::null;
}

void IrqObject::submitAwait(AwaitIrqNode *node, uint64_t sequence) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(sequence <= currentSequence());
	if(sequence < currentSequence()) {
		node->onRaise(kErrSuccess, currentSequence());
	}else{
		_waitQueue.push_back(node);
	}
}

} // namespace thor

