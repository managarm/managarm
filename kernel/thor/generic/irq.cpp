#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

namespace {
	constexpr bool logService = false;
}

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
: _name{std::move(name)}, _pin{nullptr}, _currentSequence{0} { }

void IrqSink::dumpHardwareState() {
	infoLogger() << "thor: No dump available for IRQ sink " << name() << frg::endlog;
}

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

	// Since the sink is attached in standBy state, it does not matter if the IRQ
	// is in-service or not (the sink does participate anyway).
	assert(sink->_status == IrqStatus::standBy);

	pin->_sinkList.push_back(sink);
	sink->_pin = pin;
}

Error IrqPin::ackSink(IrqSink *sink, uint64_t sequence) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);

	if(sequence != sink->currentSequence())
		return Error::illegalArgs;

	if(sink->_status != IrqStatus::indefinite)
		return Error::illegalArgs;
	sink->_status = IrqStatus::acked;
	pin->_acknowledge();
	return Error::success;
}

Error IrqPin::nackSink(IrqSink *sink, uint64_t sequence) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);

	if(sequence != sink->currentSequence())
		return Error::illegalArgs;

	if(sink->_status != IrqStatus::indefinite)
		return Error::illegalArgs;
	sink->_status = IrqStatus::nacked;
	pin->_nack();
	return Error::success;
}

Error IrqPin::kickSink(IrqSink *sink, bool wantClear) {
	auto pin = sink->getPin();
	assert(pin);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&pin->_mutex);

	// If wantClear == true, kickSink() is effictively also an ACK on the sink
	// *if the sink is indefinite right now*.
	//
	// This behavior makes it useful even if for IRQ objects that are not monitored
	// asynchronously (for example if kernlets are used instead).

	if(!wantClear || sink->_status != IrqStatus::indefinite) {
		pin->_kick(false);
		return Error::success;
	}

	if(sink->_status != IrqStatus::indefinite)
		return Error::success;
	sink->_status = IrqStatus::acked;
	pin->_kick(true);
	return Error::success;
}

// --------------------------------------------------------
// IrqPin
// --------------------------------------------------------

IrqPin::IrqPin(frg::string<KernelAlloc> name)
: _name{std::move(name)}, _strategy{IrqStrategy::null},
		_inService{false}, _dueSinks{0},
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
		dumpHardwareState();
	}else{
		assert(_strategy == IrqStrategy::justEoi
				|| _strategy == IrqStrategy::maskThenEoi);
	}

	// If the IRQ is already masked, we're encountering a hardware race.
	if(_maskState) {
		++_maskedRaiseCtr;
		// At least on x86, the IRQ controller may buffer up to one edge-triggered IRQ.
		// If an IRQ is already buffered while we mask it, it will inevitably be raised again.
		// Thus, we do not immediately complain about edge-triggered IRQs here.
		if(_strategy != IrqStrategy::justEoi || _maskedRaiseCtr > 1) {
			infoLogger() << "\e[35mthor: IRQ controller raised "
					<< _name << " despite being masked\e[39m" << frg::endlog;
			dumpHardwareState();
		}

		sendEoi();
		return;
	}

	// This can only happen for justEoi IRQs.
	// Otherwise, the IRQ is masked and the previous if would have triggered.
	if(_inService) {
		assert(_strategy == IrqStrategy::justEoi);
		_raiseBuffered = true;
		_maskState |= maskedWhileBuffered;

		_updateMask();
		sendEoi();
		return;
	}

	_doService();

	_updateMask();
	sendEoi();
}

void IrqPin::_acknowledge() {
	assert(_inService);
	assert(_dueSinks);
	_dispatchAcks = true;
	_dueSinks--;

	if(!_dueSinks)
		_dispatch();
}

void IrqPin::_nack() {
	assert(_inService);
	assert(_dueSinks);
	_dueSinks--;

	if(!_dueSinks)
		_dispatch();
}

void IrqPin::_kick(bool doClear) {
	if(doClear) {
		assert(_inService);
		assert(_dueSinks);
		_dueSinks--;
	}else{
		if(!_inService)
			return;
	}

	_dispatchAcks = true;
	_dispatchKicks = true;

	// Re-dispatch to clear the IRQ.
	// Note that we also do *not* decrement _dueSinks here; in particular, the sink that
	// was kicked might already have decremented _dueSinks.
	if(!_dueSinks)
		_dispatch();
}

// This function is called at the end of IRQ handling.
// It unmasks IRQs that use maskThenEoi and checks for asynchronous NACK.
void IrqPin::_dispatch() {
	if(_dispatchKicks)
		_maskState &= ~maskedForNack;

	if(_dispatchAcks) {
		if(logService)
			infoLogger() << "\e[37m" "thor: IRQ pin " << name()
					<< " is acked (asynchronously)" "\e[39m" << frg::endlog;

		_inService = false;
		_maskState &= ~maskedForService;

		// Avoid losing IRQs that were ignored in raise() as 'already active'.
		if(_raiseBuffered) {
			_raiseBuffered = false;
			_maskState &= ~maskedWhileBuffered;

			_doService();
		}
	}else{
		// Note that _inService returns true for NAKed IRQs.

		infoLogger() << "\e[31mthor: IRQ " << _name
				<< " was nacked (asynchronously)!\e[39m" << frg::endlog;
		for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it) {
			auto lock = frg::guard(&(*it)->_mutex);
			if((*it)->_status == IrqStatus::standBy) {
				infoLogger() << "thor: IRQ sink " << (*it)->name()
						<< " is in standBy state" << frg::endlog;
			}else{
				(*it)->dumpHardwareState();
			}
		}
		_maskState |= maskedForNack;
	}

	_updateMask();
}

void IrqPin::warnIfPending() {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	if(!_inService || (_maskState & maskedForNack))
		return;

	if(systemClockSource()->currentNanos() - _raiseClock > 1000000000 && !_warnedAfterPending) {
		auto log = infoLogger();
		log << "\e[35mthor: Pending IRQ " << _name << " has not been"
				" acked/nacked for more than one second.";
		for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it)
			if((*it)->_status == IrqStatus::indefinite)
				log << "\n   Sink " << (*it)->name() << " has not acked/nacked";
		log << "\e[39m" << frg::endlog;
		_warnedAfterPending = true;
	}
}

void IrqPin::dumpHardwareState() {
	infoLogger() << "thor: No dump available for IRQ pin " << name() << frg::endlog;
}

void IrqPin::_doService() {
	assert(!_inService);
	assert(!_raiseBuffered);

	if(logService)
		infoLogger() << "\e[37m" "thor: IRQ pin "
				<< _name << " enters service" "\e[39m" << frg::endlog;

	_inService = true;
	// maskThenEoi IRQs are masked while then are in service.
	if(_strategy == IrqStrategy::maskThenEoi)
		_maskState |= maskedForService;

	_dueSinks = 0;
	_dispatchAcks = false;
	_dispatchKicks = false;

	_raiseClock = systemClockSource()->currentNanos();
	_warnedAfterPending = false;

	if(_sinkList.empty())
		infoLogger() << "\e[35mthor: No sink for IRQ "
				<< _name << "\e[39m" << frg::endlog;

	unsigned int numAsynchronous = 0;
	bool anyAck = false;
	for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it) {
		auto lock = frg::guard(&(*it)->_mutex);
		++((*it)->_currentSequence);
		auto status = (*it)->raise();
		(*it)->_status = status;

		if(status == IrqStatus::acked) {
			anyAck = true;
		}else if(status == IrqStatus::nacked) {
			// We do not need to do anything here; we just do not increment numAsynchronous.
		}else{
			numAsynchronous++;
		}
	}

	if(!numAsynchronous) {
		_inService = false;
		_maskState &= ~maskedForService;

		if(anyAck) {
			if(logService)
				infoLogger() << "\e[37m" "thor: IRQ pin " << name()
						<< " is acked (asynchronously)" "\e[39m" << frg::endlog;
		}else{
			infoLogger() << "\e[31mthor: IRQ " << _name
					<< " was nacked (synchronously)!\e[39m" << frg::endlog;
			for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it) {
				auto lock = frg::guard(&(*it)->_mutex);
				assert((*it)->_status != IrqStatus::standBy);
				(*it)->dumpHardwareState();
			}

			_maskState |= maskedForNack;
		}
		return;
	}

	// The IRQ is handled asynchronously.

	if(anyAck)
		_dispatchAcks = true;
	_dueSinks = numAsynchronous;
}

void IrqPin::_updateMask() {
	// TODO: Avoid the virtual calls if the state does not change?
	if(!_maskState) {
		_maskedRaiseCtr = 0;
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
			infoLogger() << "thor: IRQ automation does not handle the IRQ?" << frg::endlog;
			return IrqStatus::indefinite;
		}
	}else
		return IrqStatus::indefinite;
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
