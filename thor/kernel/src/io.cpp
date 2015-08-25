
#include "kernel.hpp"

namespace thor {

namespace traits = frigg::traits;

// --------------------------------------------------------
// IrqRelay
// --------------------------------------------------------

IrqRelay::Request::Request(KernelSharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(traits::move(event_hub)), submitInfo(submit_info) { }

frigg::util::LazyInitializer<IrqRelay> irqRelays[16];

IrqRelay::IrqRelay() : p_requests(*kernelAlloc) { }

void IrqRelay::submitWaitRequest(KernelSharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info) {
	Request request(traits::move(event_hub), submit_info);
	p_requests.addBack(traits::move(request));
}

void IrqRelay::fire() {
	while(!p_requests.empty()) {
		Request request = p_requests.removeFront();
		
		UserEvent event(UserEvent::kTypeIrq, request.submitInfo);

		EventHub::Guard hub_guard(&request.eventHub->lock);
		request.eventHub->raiseEvent(hub_guard, traits::move(event));
		hub_guard.unlock();
	}
}

// --------------------------------------------------------
// IrqLine
// --------------------------------------------------------

IrqLine::IrqLine(int number) : p_number(number) { }

int IrqLine::getNumber() {
	return p_number;
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

