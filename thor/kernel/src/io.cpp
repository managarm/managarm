
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

	infoLogger->log() << "setup(" << flags << ")" << frigg::EndLog();
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
: p_number(number), p_firedSequence(0), p_notifiedSequence(0),
		p_requests(*kernelAlloc), p_subscriptions(*kernelAlloc) { }

int IrqLine::getNumber() {
	return p_number;
}

void IrqLine::submitWait(Guard &guard, KernelSharedPtr<EventHub> event_hub,
		SubmitInfo submit_info) {
	assert(!intsAreEnabled());
	assert(guard.protects(&lock));
	
	Request request(frigg::move(event_hub), submit_info);

	assert(p_firedSequence >= p_notifiedSequence);
	if(p_firedSequence > p_notifiedSequence) {
		processRequest(frigg::move(request));
	}else{
		p_requests.addBack(frigg::move(request));
	}
}

void IrqLine::subscribe(Guard &guard, KernelSharedPtr<EventHub> event_hub,
		SubmitInfo submit_info) {
	assert(!intsAreEnabled());
	assert(guard.protects(&lock));
	
	Request request(frigg::move(event_hub), submit_info);
	p_subscriptions.addBack(frigg::move(request));
}

void IrqLine::fire(Guard &guard, uint64_t sequence) {
	assert(!intsAreEnabled());
	assert(guard.protects(&lock));

	p_firedSequence = sequence;

	if(!p_requests.empty())
		processRequest(p_requests.removeFront());
	
	for(auto it = p_subscriptions.frontIter(); it.okay(); ++it) {
		UserEvent event(UserEvent::kTypeIrq, it->submitInfo);

		EventHub::Guard hub_guard(&it->eventHub->lock);
		it->eventHub->raiseEvent(hub_guard, frigg::move(event));
		hub_guard.unlock();
	}
}

void IrqLine::processRequest(Request request) {
	UserEvent event(UserEvent::kTypeIrq, request.submitInfo);

	EventHub::Guard hub_guard(&request.eventHub->lock);
	request.eventHub->raiseEvent(hub_guard, frigg::move(event));
	hub_guard.unlock();

	assert(p_firedSequence > p_notifiedSequence);
	p_notifiedSequence = p_firedSequence;
}

// --------------------------------------------------------
// IrqLine::Request
// --------------------------------------------------------

IrqLine::Request::Request(KernelSharedPtr<EventHub> event_hub, SubmitInfo submit_info)
: BaseRequest(frigg::move(event_hub), submit_info) { }

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

