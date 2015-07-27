
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

LazyInitializer<SharedPtr<Thread, KernelAlloc>> currentThread;

LazyInitializer<KernelAlloc> kernelAlloc;

void *kernelStackBase;
size_t kernelStackLength = 0x100000;

// --------------------------------------------------------
// Descriptors
// --------------------------------------------------------

IrqDescriptor::IrqDescriptor(SharedPtr<IrqLine, KernelAlloc> &&irq_line)
		: p_irqLine(util::move(irq_line)) { }

UnsafePtr<IrqLine, KernelAlloc> IrqDescriptor::getIrqLine() {
	return p_irqLine;
}

IoDescriptor::IoDescriptor(SharedPtr<IoSpace, KernelAlloc> &&io_space)
		: p_ioSpace(util::move(io_space)) { }

UnsafePtr<IoSpace, KernelAlloc> IoDescriptor::getIoSpace() {
	return p_ioSpace;
}

// --------------------------------------------------------
// Threading related functions
// --------------------------------------------------------

Universe::Universe()
		: p_descriptorMap(util::DefaultHasher<Handle>(), *kernelAlloc) { }

Handle Universe::attachDescriptor(AnyDescriptor &&descriptor) {
	Handle handle = p_nextHandle++;
	p_descriptorMap.insert(handle, util::move(descriptor));
	return handle;
}

AnyDescriptor &Universe::getDescriptor(Handle handle) {
	return p_descriptorMap.get(handle);
}

AnyDescriptor Universe::detachDescriptor(Handle handle) {
	return p_descriptorMap.remove(handle);
}

// --------------------------------------------------------
// Io
// --------------------------------------------------------

IrqRelay::Request::Request(SharedPtr<EventHub, KernelAlloc> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(util::move(event_hub)), submitInfo(submit_info) { }

LazyInitializer<IrqRelay[16]> irqRelays;

IrqRelay::IrqRelay() : p_requests(*kernelAlloc) { }

void IrqRelay::submitWaitRequest(SharedPtr<EventHub, KernelAlloc> &&event_hub,
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

void IoSpace::enableInThread(UnsafePtr<Thread, KernelAlloc> thread) {
	for(size_t i = 0; i < p_ports.size(); i++)
		thread->enableIoPort(p_ports[i]);
}

} // namespace thor

