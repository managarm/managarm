
#include "kernel.hpp"

namespace traits = frigg::traits;

namespace thor {

// --------------------------------------------------------
// MemoryAccessDescriptor
// --------------------------------------------------------

MemoryAccessDescriptor::MemoryAccessDescriptor(SharedPtr<Memory, KernelAlloc> &&memory)
		: p_memory(traits::move(memory)) { }

UnsafePtr<Memory, KernelAlloc> MemoryAccessDescriptor::getMemory() {
	return p_memory;
}

// --------------------------------------------------------
// EventHubDescriptor
// --------------------------------------------------------

EventHubDescriptor::EventHubDescriptor(SharedPtr<EventHub, KernelAlloc> &&event_hub)
		: p_eventHub(traits::move(event_hub)) { }

UnsafePtr<EventHub, KernelAlloc> EventHubDescriptor::getEventHub() {
	return p_eventHub;
}

// --------------------------------------------------------
// BiDirectionFirstDescriptor
// --------------------------------------------------------

BiDirectionFirstDescriptor::BiDirectionFirstDescriptor(SharedPtr<BiDirectionPipe, KernelAlloc> &&pipe)
		: p_pipe(traits::move(pipe)) { }

UnsafePtr<BiDirectionPipe, KernelAlloc> BiDirectionFirstDescriptor::getPipe() {
	return p_pipe;
}

// --------------------------------------------------------
// BiDirectionSecondDescriptor
// --------------------------------------------------------

BiDirectionSecondDescriptor::BiDirectionSecondDescriptor(SharedPtr<BiDirectionPipe, KernelAlloc> &&pipe)
		: p_pipe(traits::move(pipe)) { }

UnsafePtr<BiDirectionPipe, KernelAlloc> BiDirectionSecondDescriptor::getPipe() {
	return p_pipe;
}

// --------------------------------------------------------
// ServerDescriptor
// --------------------------------------------------------

ServerDescriptor::ServerDescriptor(SharedPtr<Server, KernelAlloc> &&server)
		: p_server(traits::move(server)) { }

UnsafePtr<Server, KernelAlloc> ServerDescriptor::getServer() {
	return p_server;
}

// --------------------------------------------------------
// ClientDescriptor
// --------------------------------------------------------

ClientDescriptor::ClientDescriptor(SharedPtr<Server, KernelAlloc> &&server)
		: p_server(traits::move(server)) { }

UnsafePtr<Server, KernelAlloc> ClientDescriptor::getServer() {
	return p_server;
}

// --------------------------------------------------------
// RdDescriptor
// --------------------------------------------------------

RdDescriptor::RdDescriptor(SharedPtr<RdFolder, KernelAlloc> &&folder)
		: p_folder(traits::move(folder)) { }

UnsafePtr<RdFolder, KernelAlloc> RdDescriptor::getFolder() {
	return p_folder;
}

// --------------------------------------------------------
// IrqDescriptor
// --------------------------------------------------------

IrqDescriptor::IrqDescriptor(SharedPtr<IrqLine, KernelAlloc> &&irq_line)
		: p_irqLine(traits::move(irq_line)) { }

UnsafePtr<IrqLine, KernelAlloc> IrqDescriptor::getIrqLine() {
	return p_irqLine;
}

// --------------------------------------------------------
// IoDescriptor
// --------------------------------------------------------

IoDescriptor::IoDescriptor(SharedPtr<IoSpace, KernelAlloc> &&io_space)
		: p_ioSpace(traits::move(io_space)) { }

UnsafePtr<IoSpace, KernelAlloc> IoDescriptor::getIoSpace() {
	return p_ioSpace;
}

} // namespace thor

