
namespace thor {

// --------------------------------------------------------
// Memory related descriptors
// --------------------------------------------------------

struct MemoryAccessDescriptor {
	MemoryAccessDescriptor(KernelSharedPtr<Memory> memory)
	: memory(frigg::move(memory)) { }

	KernelSharedPtr<Memory> memory;
};

struct AddressSpaceDescriptor {
	AddressSpaceDescriptor(KernelSharedPtr<AddressSpace> space)
	: space(frigg::move(space)) { }

	KernelSharedPtr<AddressSpace> space;
};

// --------------------------------------------------------
// Threading related descriptors
// --------------------------------------------------------

struct ThreadDescriptor {
	ThreadDescriptor(KernelSharedPtr<Thread> thread)
	: thread(frigg::move(thread)) { }
	
	KernelSharedPtr<Thread> thread;
};

struct SignalDescriptor {
	SignalDescriptor(KernelSharedPtr<Signal> signal)
	: signal(frigg::move(signal)) { }
	
	KernelSharedPtr<Signal> signal;
};

// --------------------------------------------------------
// Event related descriptors
// --------------------------------------------------------

struct EventHubDescriptor {
	EventHubDescriptor(KernelSharedPtr<EventHub> event_hub)
	: eventHub(frigg::move(event_hub)) { }

	KernelSharedPtr<EventHub> eventHub;
};

// --------------------------------------------------------
// IPC related descriptors
// --------------------------------------------------------

struct RingDescriptor {
	RingDescriptor(KernelSharedPtr<RingBuffer> ring_buffer)
	: ringBuffer(frigg::move(ring_buffer)) { }
	
	KernelSharedPtr<RingBuffer> ringBuffer;
};

struct EndpointDescriptor {
	EndpointDescriptor(KernelSharedPtr<Endpoint> endpoint)
	: endpoint(frigg::move(endpoint)) { }
	
	KernelSharedPtr<Endpoint> endpoint;
};

struct ServerDescriptor {
	ServerDescriptor(KernelSharedPtr<Server> server)
	: server(frigg::move(server)) { }
	
	KernelSharedPtr<Server> server;
};

struct ClientDescriptor {
	ClientDescriptor(KernelSharedPtr<Server> server)
	: server(frigg::move(server)) { }
	
	KernelSharedPtr<Server> server;
};

// --------------------------------------------------------
// Resource directory related descriptors
// --------------------------------------------------------

struct RdDescriptor {
	RdDescriptor(KernelSharedPtr<RdFolder> &&folder);
	
	KernelUnsafePtr<RdFolder> getFolder();

private:
	KernelSharedPtr<RdFolder> p_folder;
};

// --------------------------------------------------------
// IO related descriptors
// --------------------------------------------------------

struct IrqDescriptor {
	IrqDescriptor(KernelSharedPtr<IrqLine> irq_line)
	: irqLine(frigg::move(irq_line)) { }
	
	KernelSharedPtr<IrqLine> irqLine;
};

struct IoDescriptor {
	IoDescriptor(KernelSharedPtr<IoSpace> io_space)
	: ioSpace(frigg::move(io_space)) { }
	
	KernelSharedPtr<IoSpace> ioSpace;
};

// --------------------------------------------------------
// AnyDescriptor
// --------------------------------------------------------

typedef frigg::Variant<
	MemoryAccessDescriptor,
	AddressSpaceDescriptor,
	ThreadDescriptor,
	SignalDescriptor,
	EventHubDescriptor,
	RingDescriptor,
	EndpointDescriptor,
	ServerDescriptor,
	ClientDescriptor,
	RdDescriptor,
	IrqDescriptor,
	IoDescriptor
> AnyDescriptor;

} // namespace thor

