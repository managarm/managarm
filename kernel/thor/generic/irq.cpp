#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/timer.hpp>

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

IrqSink::IrqSink(frg::string<KernelAlloc> name)
: _name{std::move(name)}, _pin{nullptr}, _currentSequence{0},
		_responseSequence{0}, _status{IrqStatus::null} { }

IrqPin *IrqSink::getPin() {
	return _pin;
}

// --------------------------------------------------------
// IRQ management functions.
// --------------------------------------------------------

void IrqPin::attachSink(IrqPin *pin, IrqSink *sink) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);
	assert(!sink->_pin);

	// TODO: Decide what to do in this case.
	if(pin->_inService)
		infoLogger() << "thor: IRQ " << pin->name() << " is in service"
				" while sink is attached" << frg::endlog;

	pin->_sinkList.push_back(sink);
	sink->_pin = pin;
}

Error IrqPin::ackSink(IrqSink *sink, uint64_t sequence) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);
	assert(sink->currentSequence() == pin->_sinkSequence);

	if(sequence <= sink->_responseSequence)
		return Error::illegalArgs;
	if(sequence > sink->currentSequence())
		return Error::illegalArgs;

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
	return Error::success;
}

Error IrqPin::nackSink(IrqSink *sink, uint64_t sequence) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);
	assert(sink->currentSequence() == pin->_sinkSequence);
	
	if(sequence <= sink->_responseSequence)
		return Error::illegalArgs;
	if(sequence > sink->currentSequence())
		return Error::illegalArgs;
		
	if(sequence == sink->currentSequence()) {
		// Because _responseSequence is lagging behind, the IRQ status must be null here.
		assert(sink->_status == IrqStatus::null);
		sink->_status = IrqStatus::nacked;
		pin->_nack();
	}
	sink->_responseSequence = sequence;

	return Error::success;
}

Error IrqPin::kickSink(IrqSink *sink) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);
	
	pin->_kick();
	return Error::success;
}

// --------------------------------------------------------
// IrqPin
// --------------------------------------------------------

IrqPin::IrqPin(frg::string<KernelAlloc> name)
: _name{std::move(name)}, _strategy{IrqStrategy::null},
		_raiseSequence{0}, _sinkSequence{0}, _inService{false}, _dueSinks{0},
		_maskState{0} { }

void IrqPin::configure(IrqConfiguration desired) {
	assert(desired.specified());

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	if(!_activeCfg.specified()) {
		infoLogger() << "thor: Configuring IRQ " << _name
				<< " to trigger mode: " << static_cast<int>(desired.trigger)
				<< ", polarity: " << static_cast<int>(desired.polarity) << frg::endlog;
		_strategy = program(desired.trigger, desired.polarity);

		_activeCfg = desired;
		_raiseSequence = 0;
		_sinkSequence = 0;
		_inService = false;
		_dueSinks = 0;
		_maskState = 0;
	}else{
		assert(_activeCfg.compatible(desired));
	}
}

void IrqPin::raise() {
	assert(!intsAreEnabled());
	auto lock = frg::guard(&_mutex);
	
	if(_strategy == IrqStrategy::null) {
		infoLogger() << "\e[35mthor: Unconfigured IRQ was raised\e[39m" << frg::endlog;
	}else{
		assert(_strategy == IrqStrategy::justEoi
				|| _strategy == IrqStrategy::maskThenEoi);
	}

	// If the IRQ is already masked, we're encountering a hardware race.
	if(_maskState) {
		infoLogger() << "\e[31mthor: Ignoring masked IRQ " << _name
				<< " (hardware race?).\e[39m" << frg::endlog;
		sendEoi();
		return;
	}

	auto already_in_service = _inService;
	_raiseSequence++;
	_inService = true;
	
	if(already_in_service) {
		assert(_strategy == IrqStrategy::justEoi);
		_maskState |= maskedForService;
	}else{
		_callSinks();

		if(_inService && !_dueSinks) {
			infoLogger() << "\e[31mthor: IRQ " << _name
					<< " was nacked (synchronously)!\e[39m" << frg::endlog;
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

	infoLogger() << "\e[31mthor: IRQ " << _name
			<< " was nacked (asynchronously)!\e[39m" << frg::endlog;
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
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	if(!_inService || !_dueSinks)
		return;

	if(systemClockSource()->currentNanos() - _raiseClock > 1000000000 && !_warnedAfterPending) {
		auto log = infoLogger();
		log << "\e[35mthor: Pending IRQ " << _name << " has not been"
				" acked/nacked for more than one second.";
		for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it)
			if((*it)->_status == IrqStatus::null)
				log << "\n   Sink " << (*it)->name() << " has not acked/nacked";
		log << "\e[39m" << frg::endlog;
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
		infoLogger() << "\e[35mthor: No sink for IRQ "
				<< _name << "\e[39m" << frg::endlog;

	for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it) {
		auto lock = frg::guard(&(*it)->_mutex);
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
IrqObject::IrqObject(frg::string<KernelAlloc> name)
: IrqSink{std::move(name)} { }

// TODO: Add a sequence parameter to this function and run the kernlet if the sequence advanced.
//       This would prevent races between automate() and IRQs.
void IrqObject::automate(smarter::shared_ptr<BoundKernlet> kernlet) {
	_automationKernlet = std::move(kernlet);
}

IrqStatus IrqObject::raise() {
	while(!_waitQueue.empty()) {
		auto node = _waitQueue.pop_front();
		node->_error = Error::success;
		node->_sequence = currentSequence();
		WorkQueue::post(node->_awaited);
	}

	if(_automationKernlet) {
		auto result = _automationKernlet->invokeIrqAutomation();
		if(result == 1) {
			return IrqStatus::acked;
		}else if(result == 2) {
			return IrqStatus::nacked;
		}else{
			assert(!result);
			return IrqStatus::null;
		}
	}else
		return IrqStatus::null;
}

void IrqObject::submitAwait(AwaitIrqNode *node, uint64_t sequence) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(sinkMutex());

	assert(sequence <= currentSequence());
	if(sequence < currentSequence()) {
		node->_error = Error::success;
		node->_sequence = currentSequence();
		WorkQueue::post(node->_awaited);
	}else{
		_waitQueue.push_back(node);
	}
}

} // namespace thor
