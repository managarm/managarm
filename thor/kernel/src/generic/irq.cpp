
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

void IrqPin::raise() {
	auto guard = frigg::guard(&_mutex);

	sendEoi();

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

void IrqObject::raise() {
	if(!_waitQueue.empty()) {
		auto wait = _waitQueue.removeFront();
		wait->complete(kErrSuccess);
	}
}

void IrqObject::submitAwait(frigg::SharedPtr<AwaitIrqBase> wait) {
	_waitQueue.addBack(frigg::move(wait));
}

void IrqObject::acknowledge() {
	assert(!"Implement this");
}

} // namespace thor

