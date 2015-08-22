
#include "kernel.hpp"

namespace traits = frigg::traits;

namespace thor {

// --------------------------------------------------------
// MemoryAccessDescriptor
// --------------------------------------------------------

MemoryAccessDescriptor::MemoryAccessDescriptor(KernelSharedPtr<Memory> &&memory)
		: p_memory(traits::move(memory)) { }

KernelUnsafePtr<Memory> MemoryAccessDescriptor::getMemory() {
	return p_memory;
}

// --------------------------------------------------------
// AddressSpaceDescriptor
// --------------------------------------------------------

AddressSpaceDescriptor::AddressSpaceDescriptor(KernelSharedPtr<AddressSpace> &&space)
		: p_space(traits::move(space)) { }

KernelUnsafePtr<AddressSpace> AddressSpaceDescriptor::getSpace() {
	return p_space;
}

// --------------------------------------------------------
// EventHubDescriptor
// --------------------------------------------------------

EventHubDescriptor::EventHubDescriptor(KernelSharedPtr<EventHub> &&event_hub)
		: p_eventHub(traits::move(event_hub)) { }

KernelUnsafePtr<EventHub> EventHubDescriptor::getEventHub() {
	return p_eventHub;
}

// --------------------------------------------------------
// BiDirectionFirstDescriptor
// --------------------------------------------------------

BiDirectionFirstDescriptor::BiDirectionFirstDescriptor(KernelSharedPtr<BiDirectionPipe> &&pipe)
		: p_pipe(traits::move(pipe)) { }

KernelUnsafePtr<BiDirectionPipe> BiDirectionFirstDescriptor::getPipe() {
	return p_pipe;
}

// --------------------------------------------------------
// BiDirectionSecondDescriptor
// --------------------------------------------------------

BiDirectionSecondDescriptor::BiDirectionSecondDescriptor(KernelSharedPtr<BiDirectionPipe> &&pipe)
		: p_pipe(traits::move(pipe)) { }

KernelUnsafePtr<BiDirectionPipe> BiDirectionSecondDescriptor::getPipe() {
	return p_pipe;
}

// --------------------------------------------------------
// ServerDescriptor
// --------------------------------------------------------

ServerDescriptor::ServerDescriptor(KernelSharedPtr<Server> &&server)
		: p_server(traits::move(server)) { }

KernelUnsafePtr<Server> ServerDescriptor::getServer() {
	return p_server;
}

// --------------------------------------------------------
// ClientDescriptor
// --------------------------------------------------------

ClientDescriptor::ClientDescriptor(KernelSharedPtr<Server> &&server)
		: p_server(traits::move(server)) { }

KernelUnsafePtr<Server> ClientDescriptor::getServer() {
	return p_server;
}

// --------------------------------------------------------
// RdDescriptor
// --------------------------------------------------------

RdDescriptor::RdDescriptor(KernelSharedPtr<RdFolder> &&folder)
		: p_folder(traits::move(folder)) { }

KernelUnsafePtr<RdFolder> RdDescriptor::getFolder() {
	return p_folder;
}

// --------------------------------------------------------
// IrqDescriptor
// --------------------------------------------------------

IrqDescriptor::IrqDescriptor(KernelSharedPtr<IrqLine> &&irq_line)
		: p_irqLine(traits::move(irq_line)) { }

KernelUnsafePtr<IrqLine> IrqDescriptor::getIrqLine() {
	return p_irqLine;
}

// --------------------------------------------------------
// IoDescriptor
// --------------------------------------------------------

IoDescriptor::IoDescriptor(KernelSharedPtr<IoSpace> &&io_space)
		: p_ioSpace(traits::move(io_space)) { }

KernelUnsafePtr<IoSpace> IoDescriptor::getIoSpace() {
	return p_ioSpace;
}

} // namespace thor

