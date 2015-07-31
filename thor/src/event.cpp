
#include "kernel.hpp"

namespace traits = frigg::traits;

namespace thor {

// --------------------------------------------------------
// SubmitInfo
// --------------------------------------------------------

SubmitInfo::SubmitInfo(int64_t submit_id,
		uintptr_t submit_function, uintptr_t submit_object)
	: submitId(submit_id), submitFunction(submit_function),
		submitObject(submit_object) { }

// --------------------------------------------------------
// EventHub
// --------------------------------------------------------

EventHub::EventHub() : p_queue(*kernelAlloc) { }

void EventHub::raiseIrqEvent(SubmitInfo submit_info) {
	Event event(Event::kTypeIrq, submit_info);
	p_queue.addBack(traits::move(event));
}

void EventHub::raiseRecvStringErrorEvent(Error error,
		SubmitInfo submit_info) {
	Event event(Event::kTypeRecvStringError, submit_info);
	event.error = error;
	p_queue.addBack(traits::move(event));
}

void EventHub::raiseRecvStringTransferEvent(uint8_t *kernel_buffer,
		uint8_t *user_buffer, size_t length, SubmitInfo submit_info) {
	Event event(Event::kTypeRecvStringTransfer, submit_info);
	event.kernelBuffer = kernel_buffer;
	event.userBuffer = user_buffer;
	event.length = length;
	p_queue.addBack(traits::move(event));
}

void EventHub::raiseAcceptEvent(SharedPtr<BiDirectionPipe, KernelAlloc> &&pipe,
		SubmitInfo submit_info) {
	Event event(Event::kTypeAccept, submit_info);
	event.pipe = traits::move(pipe);
	p_queue.addBack(traits::move(event));
}

void EventHub::raiseConnectEvent(SharedPtr<BiDirectionPipe, KernelAlloc> &&pipe,
		SubmitInfo submit_info) {
	Event event(Event::kTypeConnect, submit_info);
	event.pipe = traits::move(pipe);
	p_queue.addBack(traits::move(event));
}

bool EventHub::hasEvent() {
	return !p_queue.empty();
}

EventHub::Event EventHub::dequeueEvent() {
	return p_queue.removeFront();
}

// --------------------------------------------------------
// EventHub::Event
// --------------------------------------------------------

EventHub::Event::Event(Type type, SubmitInfo submit_info)
		: type(type), submitInfo(submit_info) { }

} // namespace thor

