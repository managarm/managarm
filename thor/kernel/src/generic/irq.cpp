
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
: _name{frigg::move(name)}, _pin{nullptr}, _currentSequence{0},
		_responseSequence{0}, _status{IrqStatus::null} { }

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

	// TODO: Decide what to do in this case.
	if(pin->_inService)
		frigg::infoLogger() << "thor: IRQ " << pin->name() << " is in service"
				" while sink is attached" << frigg::endLog;

	pin->_sinkList.push_back(sink);
	sink->_pin = pin;
}

Error IrqPin::ackSink(IrqSink *sink, uint64_t sequence) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&pin->_mutex);
	assert(sink->currentSequence() == pin->_sinkSequence);

	if(sequence <= sink->_responseSequence)
		return kErrIllegalArgs;
	if(sequence > sink->currentSequence())
		return kErrIllegalArgs;

	if(sequence == sink->currentSequence()) {
		// Because _responseSequence is lagging behind, the IRQ status must be null here.
		assert(sink->_status == IrqStatus::null);
		sink->_status = IrqStatus::acked;
	}
	sink->_responseSequence = sequence;

	// Note that we have to unblock the IRQ regardless of whether the ACK targets the
	// currentSequence(). That avoids a race in the following scenario:
	// Device A: Generates IRQ.
	// Device B: Generates IRQ.
	// IrqPin is raise()ed.
	// Device A: Handles IRQ and ACKs.
	// IrqPin is unmask()ed.
	// IrqPin is raise()ed and mask()ed.
	// Device B: Handles IRQ and ACKs.
	// Now, the IrqPin is needs to be unmask()ed again, even though the ACK sequence
	// does not necessarily match the currentSequence().
	pin->_acknowledge();
	return kErrSuccess;
}

Error IrqPin::nackSink(IrqSink *sink, uint64_t sequence) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&pin->_mutex);
	assert(sink->currentSequence() == pin->_sinkSequence);
	
	if(sequence <= sink->_responseSequence)
		return kErrIllegalArgs;
	if(sequence > sink->currentSequence())
		return kErrIllegalArgs;
		
	if(sequence == sink->currentSequence()) {
		// Because _responseSequence is lagging behind, the IRQ status must be null here.
		assert(sink->_status == IrqStatus::null);
		sink->_status = IrqStatus::nacked;
		pin->_nack();
	}
	sink->_responseSequence = sequence;

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
	assert(!intsAreEnabled());
	auto lock = frigg::guard(&_mutex);
	
	if(_strategy == IrqStrategy::null) {
		frigg::infoLogger() << "\e[35mthor: Unconfigured IRQ was raised\e[39m" << frigg::endLog;
	}else{
		assert(_strategy == IrqStrategy::justEoi
				|| _strategy == IrqStrategy::maskThenEoi);
	}

	// If the IRQ is already masked, we're encountering a hardware race.
	assert(!_maskState);

	auto already_in_service = _inService;
	_raiseSequence++;
	_inService = true;
	
	if(already_in_service) {
		assert(_strategy == IrqStrategy::justEoi);
		_maskState |= maskedForService;
	}else{
		_callSinks();

		if(_inService && !_dueSinks) {
			frigg::infoLogger() << "\e[31mthor: IRQ " << _name
					<< " was nacked (synchronously)!\e[39m" << frigg::endLog;
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

	frigg::infoLogger() << "\e[31mthor: IRQ " << _name
			<< " was nacked (asynchronously)!\e[39m" << frigg::endLog;
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

	if(systemClockSource()->currentNanos() - _raiseClock > 1000000000 && !_warnedAfterPending) {
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
		_raiseClock = systemClockSource()->currentNanos();
		_warnedAfterPending = false;
	}

	if(_sinkList.empty())
		frigg::infoLogger() << "\e[35mthor: No sink for IRQ "
				<< _name << "\e[39m" << frigg::endLog;

	for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it) {
		auto lock = frigg::guard(&(*it)->_mutex);
		(*it)->_currentSequence = _sinkSequence;
		auto status = (*it)->raise();

		(*it)->_status = status;
		if(status != IrqStatus::null)
			(*it)->_responseSequence = _sinkSequence;

		if(status == IrqStatus::acked) {
			_inService = false;
		}else if(status == IrqStatus::nacked) {
			// We do not need to do anything here; we just do not increment _dueSinks.
		}else{
			_dueSinks++;
		}
	}
}

void IrqPin::_updateMask() {
	// TODO: Avoid the virtual calls if the state does not change?
	if(!_maskState) {
		unmask();
	}else{
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

// TODO: Add a sequence parameter to this function and run the kernlet if the sequence advanced.
//       This would prevent races between automate() and IRQs.
void IrqObject::automate(frigg::SharedPtr<BoundKernlet> kernlet) {
	_automationKernlet = std::move(kernlet);
}

IrqStatus IrqObject::raise() {
	while(!_waitQueue.empty()) {
		auto node = _waitQueue.pop_front();
		node->_error = kErrSuccess;
		node->_sequence = currentSequence();
		WorkQueue::post(node->_awaited);
	}

	if(_automationKernlet) {
		if(!_automationKernlet->invokeIrqAutomation())
			return IrqStatus::nacked;
		return IrqStatus::acked;
	}else
		return IrqStatus::null;
}

void IrqObject::submitAwait(AwaitIrqNode *node, uint64_t sequence) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(sinkMutex());

	assert(sequence <= currentSequence());
	if(sequence < currentSequence()) {
		node->_error = kErrSuccess;
		node->_sequence = currentSequence();
		WorkQueue::post(node->_awaited);
	}else{
		_waitQueue.push_back(node);
	}
}

} // namespace thor

