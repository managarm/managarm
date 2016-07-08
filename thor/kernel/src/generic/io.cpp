
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// IrqRelay
// --------------------------------------------------------

frigg::LazyInitializer<IrqRelay> irqRelays[16];

IrqRelay::IrqRelay()
: p_flags(0), p_sequence(0), p_lines(*kernelAlloc) { }

void IrqRelay::addLine(Guard &guard, frigg::WeakPtr<IrqLine> line) {
	assert(!intsAreEnabled());
	assert(guard.protects(&lock));

	p_lines.push(frigg::move(line));
}

void IrqRelay::setup(Guard&guard, uint32_t flags) {
	assert(!intsAreEnabled());
	assert(guard.protects(&lock));

	frigg::infoLogger.log() << "setup(" << flags << ")" << frigg::EndLog();
	p_flags = flags;
}

void IrqRelay::fire(Guard &guard) {
	assert(!intsAreEnabled());
	assert(guard.protects(&lock));

	p_sequence++;

	for(size_t i = 0; i < p_lines.size(); i++) {
		frigg::SharedPtr<IrqLine> line = p_lines[i].grab();
		if(!line)
			continue; // TODO: remove the irq line

		IrqLine::Guard line_guard(&line->lock);
		line->fire(line_guard, p_sequence);
	}
	
	if(!(p_flags & kFlagManualAcknowledge))
		acknowledgeIrq(0); // FIXME
}

void IrqRelay::manualAcknowledge(Guard &guard) {
	assert(!intsAreEnabled());
	assert(guard.protects(&lock));

	assert(p_flags & kFlagManualAcknowledge);
	acknowledgeIrq(0); // FIXME
}

// --------------------------------------------------------
// IrqLine
// --------------------------------------------------------

IrqLine::IrqLine(int number)
: _number(number), _firedSequence(0), _notifiedSequence(0) { }

int IrqLine::getNumber() {
	return _number;
}

void IrqLine::submitWait(Guard &guard, frigg::SharedPtr<AsyncIrq> wait) {
	assert(!intsAreEnabled());
	assert(guard.protects(&lock));

	assert(_firedSequence >= _notifiedSequence);
	if(_firedSequence > _notifiedSequence) {
		processWait(frigg::move(wait));
	}else{
		_waitQueue.addBack(frigg::move(wait));
	}
}

void IrqLine::fire(Guard &guard, uint64_t sequence) {
	assert(!intsAreEnabled());
	assert(guard.protects(&lock));

	_firedSequence = sequence;

	if(!_waitQueue.empty())
		processWait(_waitQueue.removeFront());
}

void IrqLine::processWait(frigg::SharedPtr<AsyncIrq> wait) {
	assert(_firedSequence > _notifiedSequence);
	_notifiedSequence = _firedSequence;

	AsyncOperation::complete(frigg::move(wait));
}

// --------------------------------------------------------
// IoSpace
// --------------------------------------------------------

IoSpace::IoSpace() : p_ports(*kernelAlloc) { }

void IoSpace::addPort(uintptr_t port) {
	p_ports.push(port);
}

void IoSpace::enableInThread(KernelUnsafePtr<Thread> thread) {
	for(size_t i = 0; i < p_ports.size(); i++)
		thread->enableIoPort(p_ports[i]);
}

} // namespace thor

