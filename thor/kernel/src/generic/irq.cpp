
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
: _name{std::move(name)}, _strategy{IrqStrategy::null} { }

void IrqPin::configure(TriggerMode mode, Polarity polarity) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	frigg::infoLogger() << "thor: Configuring IRQ " << _name
			<< " to trigger mode: " << static_cast<int>(mode)
			<< ", polarity: " << static_cast<int>(polarity) << frigg::endLog;

	_strategy = program(mode, polarity);
	_latched = false;
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

	if(!_latched) {
		auto status = _callSinks();

		// Latch this IRQ.
		if(!(status & irq_status::handled)) {
			_latched = true;
			_raiseClock = currentNanos();
			_warnedAfterPending = false;

			if(_strategy == IrqStrategy::maskThenEoi)
				mask();
		}
	}else{
		// TODO: If the IRQ is edge-triggered we lose an edge here!
		frigg::infoLogger() << "\e[35mthor: Ignoring already latched IRQ " << _name
				<< "\e[39m" << frigg::endLog;
	}

	sendEoi();
}

void IrqPin::kick() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	if(!_latched)
		return;

	if(_strategy == IrqStrategy::maskThenEoi) {
		unmask();
	}else{
		assert(_strategy == IrqStrategy::justEoi);
	}

	_latched = false;
}

void IrqPin::acknowledge() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	if(!_latched)
		return;

	if(_strategy == IrqStrategy::maskThenEoi) {
		unmask();
	}else{
		assert(_strategy == IrqStrategy::justEoi);
	}

	_latched = false;
}

void IrqPin::warnIfPending() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	if(_latched && currentNanos() - _raiseClock > 1000000000 && !_warnedAfterPending) {
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
		status |= (*it)->raise();
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
: _latched{true} { }

IrqStatus IrqObject::raise() {
	if(_waitQueue.empty()) {
		_latched = true;
	}else{
		assert(!_latched);

		while(!_waitQueue.empty()) {
			auto wait = _waitQueue.removeFront();
			wait->onRaise(kErrSuccess);
		}
	}

	return irq_status::null;
}

void IrqObject::submitAwait(frigg::SharedPtr<AwaitIrqNode> wait) {
	if(_latched) {
		wait->onRaise(kErrSuccess);
		_latched = false;
	}else{
		_waitQueue.addBack(frigg::move(wait));
	}
}

void IrqObject::acknowledge() {
	getPin()->acknowledge();
}

} // namespace thor

