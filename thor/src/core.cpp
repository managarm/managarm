
#include "../../frigg/include/types.hpp"
#include "util/general.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "util/vector.hpp"
#include "util/smart-ptr.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"
#include "core.hpp"

namespace thor {

LazyInitializer<SharedPtr<Thread>> currentThread;

LazyInitializer<KernelAlloc> kernelAlloc;

// --------------------------------------------------------
// Descriptors
// --------------------------------------------------------

IrqDescriptor::IrqDescriptor(SharedPtr<IrqLine> &&irq_line)
		: p_irqLine(util::move(irq_line)) { }

UnsafePtr<IrqLine> IrqDescriptor::getIrqLine() {
	return p_irqLine->unsafe<IrqLine>();
}

IoDescriptor::IoDescriptor(SharedPtr<IoSpace> &&io_space)
		: p_ioSpace(util::move(io_space)) { }

UnsafePtr<IoSpace> IoDescriptor::getIoSpace() {
	return p_ioSpace->unsafe<IoSpace>();
}

// --------------------------------------------------------
// Threading related functions
// --------------------------------------------------------

Universe::Universe()
		: p_descriptorMap(util::DefaultHasher<Handle>(), *kernelAlloc) { }

AnyDescriptor &Universe::getDescriptor(Handle handle) {
	return p_descriptorMap.get(handle);
}


// --------------------------------------------------------
// Io
// --------------------------------------------------------

IrqRelay::Request::Request(SharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(util::move(event_hub)), submitInfo(submit_info) { }

LazyInitializer<IrqRelay[16]> irqRelays;

IrqRelay::IrqRelay() : p_requests(*kernelAlloc) { }

void IrqRelay::submitWaitRequest(SharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info) {
	Request request(util::move(event_hub), submit_info);
	p_requests.addBack(util::move(request));
}

void IrqRelay::fire() {
	while(!p_requests.empty()) {
		Request request = p_requests.removeFront();
		request.eventHub->raiseIrqEvent(request.submitInfo);
	}
}


IrqLine::IrqLine(int number) : p_number(number) { }

int IrqLine::getNumber() {
	return p_number;
}


IoSpace::IoSpace() : p_ports(*kernelAlloc) { }

void IoSpace::addPort(uintptr_t port) {
	p_ports.push(port);
}

void IoSpace::enableInThread(UnsafePtr<Thread> thread) {
	for(size_t i = 0; i < p_ports.size(); i++)
		thread->enableIoPort(p_ports[i]);
}

} // namespace thor

void *operator new(size_t length, thor::KernelAlloc *allocator) {
	return allocator->allocate(length);
}

