
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

// --------------------------------------------------------
// MemoryAccessDescriptor
// --------------------------------------------------------

MemoryAccessDescriptor::MemoryAccessDescriptor(SharedPtr<Memory, KernelAlloc> &&memory)
		: p_memory(util::move(memory)) { }

UnsafePtr<Memory, KernelAlloc> MemoryAccessDescriptor::getMemory() {
	return p_memory;
}

// --------------------------------------------------------
// EventHubDescriptor
// --------------------------------------------------------

EventHubDescriptor::EventHubDescriptor(SharedPtr<EventHub, KernelAlloc> &&event_hub)
		: p_eventHub(util::move(event_hub)) { }

UnsafePtr<EventHub, KernelAlloc> EventHubDescriptor::getEventHub() {
	return p_eventHub;
}

// --------------------------------------------------------
// BiDirectionFirstDescriptor
// --------------------------------------------------------

BiDirectionFirstDescriptor::BiDirectionFirstDescriptor(SharedPtr<BiDirectionPipe, KernelAlloc> &&pipe)
		: p_pipe(util::move(pipe)) { }

UnsafePtr<BiDirectionPipe, KernelAlloc> BiDirectionFirstDescriptor::getPipe() {
	return p_pipe;
}

// --------------------------------------------------------
// BiDirectionSecondDescriptor
// --------------------------------------------------------

BiDirectionSecondDescriptor::BiDirectionSecondDescriptor(SharedPtr<BiDirectionPipe, KernelAlloc> &&pipe)
		: p_pipe(util::move(pipe)) { }

UnsafePtr<BiDirectionPipe, KernelAlloc> BiDirectionSecondDescriptor::getPipe() {
	return p_pipe;
}

// --------------------------------------------------------
// ServerDescriptor
// --------------------------------------------------------

ServerDescriptor::ServerDescriptor(SharedPtr<Server, KernelAlloc> &&server)
		: p_server(util::move(server)) { }

UnsafePtr<Server, KernelAlloc> ServerDescriptor::getServer() {
	return p_server;
}

// --------------------------------------------------------
// ClientDescriptor
// --------------------------------------------------------

ClientDescriptor::ClientDescriptor(SharedPtr<Server, KernelAlloc> &&server)
		: p_server(util::move(server)) { }

UnsafePtr<Server, KernelAlloc> ClientDescriptor::getServer() {
	return p_server;
}

// --------------------------------------------------------
// RdDescriptor
// --------------------------------------------------------

RdDescriptor::RdDescriptor(SharedPtr<RdFolder, KernelAlloc> &&folder)
		: p_folder(util::move(folder)) { }

UnsafePtr<RdFolder, KernelAlloc> RdDescriptor::getFolder() {
	return p_folder;
}

// --------------------------------------------------------
// IrqDescriptor
// --------------------------------------------------------

IrqDescriptor::IrqDescriptor(SharedPtr<IrqLine, KernelAlloc> &&irq_line)
		: p_irqLine(util::move(irq_line)) { }

UnsafePtr<IrqLine, KernelAlloc> IrqDescriptor::getIrqLine() {
	return p_irqLine;
}

// --------------------------------------------------------
// IoDescriptor
// --------------------------------------------------------

IoDescriptor::IoDescriptor(SharedPtr<IoSpace, KernelAlloc> &&io_space)
		: p_ioSpace(util::move(io_space)) { }

UnsafePtr<IoSpace, KernelAlloc> IoDescriptor::getIoSpace() {
	return p_ioSpace;
}

} // namespace thor

