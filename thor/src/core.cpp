
#include "kernel.hpp"

namespace traits = frigg::traits;
namespace util = frigg::util;

namespace thor {

// --------------------------------------------------------
// Debugging and logging
// --------------------------------------------------------

BochsSink infoSink;
LazyInitializer<frigg::debug::DefaultLogger<BochsSink>> infoLogger;

UnsafePtr<Thread, KernelAlloc> getCurrentThread() {
	auto cpu_context = (CpuContext *)thorRtGetCpuContext();
	return cpu_context->currentThread;
}

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

KernelVirtualAlloc::KernelVirtualAlloc()
: p_nextPage(0xFFFF800200000000) { }

uintptr_t KernelVirtualAlloc::map(size_t length) {
	ASSERT((length % kPageSize) == 0);
	uintptr_t address = p_nextPage;
	p_nextPage += length;

	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = physicalAllocator->allocate(1);
		kernelSpace->mapSingle4k(address + offset, physical, false,
				PageSpace::kAccessWrite);
	}
	thorRtInvalidateSpace();
	asm("" : : : "memory");

	return address;
}

void KernelVirtualAlloc::unmap(uintptr_t address, size_t length) {
	ASSERT((address % kPageSize) == 0);
	ASSERT((length % kPageSize) == 0);

	asm("" : : : "memory");
	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = kernelSpace->unmapSingle4k(address + offset);
		physicalAllocator->free(physical);
	}
	thorRtInvalidateSpace();
}

LazyInitializer<PhysicalChunkAllocator> physicalAllocator;
LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;
LazyInitializer<KernelAlloc> kernelAlloc;

void *kernelStackBase;
size_t kernelStackLength = 0x100000;


// --------------------------------------------------------
// Threading related functions
// --------------------------------------------------------

Universe::Universe()
		: p_descriptorMap(util::DefaultHasher<Handle>(), *kernelAlloc) { }

Handle Universe::attachDescriptor(AnyDescriptor &&descriptor) {
	Handle handle = p_nextHandle++;
	p_descriptorMap.insert(handle, traits::move(descriptor));
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
	: eventHub(traits::move(event_hub)), submitInfo(submit_info) { }

LazyInitializer<IrqRelay[16]> irqRelays;

IrqRelay::IrqRelay() : p_requests(*kernelAlloc) { }

void IrqRelay::submitWaitRequest(SharedPtr<EventHub, KernelAlloc> &&event_hub,
		SubmitInfo submit_info) {
	Request request(traits::move(event_hub), submit_info);
	p_requests.addBack(traits::move(request));
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

void friggPrintCritical(char c) {
	thor::infoSink.print(c);
}
void friggPrintCritical(char const *str) {
	thor::infoSink.print(str);
}
void friggPanic() {
	thorRtHalt();
}


