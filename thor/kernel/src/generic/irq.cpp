
#include "irq.hpp"

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

IrqPin::IrqPin()
: _strategy{IrqStrategy::null} { }

void IrqPin::configure(TriggerMode mode, Polarity polarity) {
	auto guard = frigg::guard(&_mutex);
	_strategy = program(mode, polarity);
}

void IrqPin::raise() {
	auto guard = frigg::guard(&_mutex);

	if(_strategy == IrqStrategy::justEoi) {
		sendEoi();
		_callSinks();
	}else if(_strategy == IrqStrategy::maskThenEoi) {
		mask();
		sendEoi();
		_callSinks();
	}else{
		assert(_strategy == IrqStrategy::null);
		frigg::infoLogger() << "\e[35mthor: Unconfigured IRQ was raised\e[39m" << frigg::endLog;
	}
}

void IrqPin::acknowledge() {
	auto guard = frigg::guard(&_mutex);

	if(_strategy == IrqStrategy::maskThenEoi) {
		unmask();
	}else{
		assert(_strategy == IrqStrategy::justEoi);
	}
}

void IrqPin::_callSinks() {
	if(_sinkList.empty())
		frigg::infoLogger() << "\e[35mthor: No sink for IRQ\e[39m" << frigg::endLog;

	for(auto it = _sinkList.begin(); it != _sinkList.end(); ++it)
		(*it)->raise();
}

void attachIrq(IrqPin *pin, IrqSink *sink) {
	auto guard = frigg::guard(&pin->_mutex);

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

void IrqObject::raise() {
	if(_waitQueue.empty()) {
		_latched = true;
	}else{
		assert(!_latched);

		while(!_waitQueue.empty()) {
			auto wait = _waitQueue.removeFront();
			wait->complete(kErrSuccess);
		}
	}
}

void IrqObject::submitAwait(frigg::SharedPtr<AwaitIrqBase> wait) {
	if(_latched) {
		wait->complete(kErrSuccess);
		_latched = false;
	}else{
		_waitQueue.addBack(frigg::move(wait));
	}
}

void IrqObject::acknowledge() {
	getPin()->acknowledge();
}

} // namespace thor

