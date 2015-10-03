
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// MemoryAccessDescriptor
// --------------------------------------------------------

MemoryAccessDescriptor::MemoryAccessDescriptor(KernelSharedPtr<Memory> &&memory)
		: p_memory(frigg::move(memory)) { }

KernelUnsafePtr<Memory> MemoryAccessDescriptor::getMemory() {
	return p_memory;
}

// --------------------------------------------------------
// AddressSpaceDescriptor
// --------------------------------------------------------

AddressSpaceDescriptor::AddressSpaceDescriptor(KernelSharedPtr<AddressSpace> &&space)
		: p_space(frigg::move(space)) { }

KernelUnsafePtr<AddressSpace> AddressSpaceDescriptor::getSpace() {
	return p_space;
}

// --------------------------------------------------------
// EventHubDescriptor
// --------------------------------------------------------

EventHubDescriptor::EventHubDescriptor(KernelSharedPtr<EventHub> &&event_hub)
		: p_eventHub(frigg::move(event_hub)) { }

KernelUnsafePtr<EventHub> EventHubDescriptor::getEventHub() {
	return p_eventHub;
}

// --------------------------------------------------------
// EndpointDescriptor
// --------------------------------------------------------

EndpointDescriptor::EndpointDescriptor(KernelSharedPtr<Endpoint> &&endpoint)
		: p_endpoint(frigg::move(endpoint)) { }

KernelUnsafePtr<Endpoint> EndpointDescriptor::getEndpoint() {
	return p_endpoint;
}

// --------------------------------------------------------
// ServerDescriptor
// --------------------------------------------------------

ServerDescriptor::ServerDescriptor(KernelSharedPtr<Server> &&server)
		: p_server(frigg::move(server)) { }

KernelUnsafePtr<Server> ServerDescriptor::getServer() {
	return p_server;
}

// --------------------------------------------------------
// ClientDescriptor
// --------------------------------------------------------

ClientDescriptor::ClientDescriptor(KernelSharedPtr<Server> &&server)
		: p_server(frigg::move(server)) { }

KernelUnsafePtr<Server> ClientDescriptor::getServer() {
	return p_server;
}

// --------------------------------------------------------
// RdDescriptor
// --------------------------------------------------------

RdDescriptor::RdDescriptor(KernelSharedPtr<RdFolder> &&folder)
		: p_folder(frigg::move(folder)) { }

KernelUnsafePtr<RdFolder> RdDescriptor::getFolder() {
	return p_folder;
}

// --------------------------------------------------------
// IrqDescriptor
// --------------------------------------------------------

IrqDescriptor::IrqDescriptor(KernelSharedPtr<IrqLine> &&irq_line)
		: p_irqLine(frigg::move(irq_line)) { }

KernelUnsafePtr<IrqLine> IrqDescriptor::getIrqLine() {
	return p_irqLine;
}

// --------------------------------------------------------
// IoDescriptor
// --------------------------------------------------------

IoDescriptor::IoDescriptor(KernelSharedPtr<IoSpace> &&io_space)
		: p_ioSpace(frigg::move(io_space)) { }

KernelUnsafePtr<IoSpace> IoDescriptor::getIoSpace() {
	return p_ioSpace;
}

} // namespace thor

