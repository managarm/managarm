
namespace thor {

// --------------------------------------------------------
// Memory related descriptors
// --------------------------------------------------------

class MemoryAccessDescriptor {
public:
	MemoryAccessDescriptor(KernelSharedPtr<Memory> &&memory);

	KernelUnsafePtr<Memory> getMemory();

private:
	KernelSharedPtr<Memory> p_memory;
};

class AddressSpaceDescriptor {
public:
	AddressSpaceDescriptor(KernelSharedPtr<AddressSpace> &&space);

	KernelUnsafePtr<AddressSpace> getSpace();

private:
	KernelSharedPtr<AddressSpace> p_space;
};

// --------------------------------------------------------
// Threading related descriptors
// --------------------------------------------------------

class ThreadDescriptor {
public:
	ThreadDescriptor(KernelSharedPtr<Thread> &&thread);
	
	KernelSharedPtr<Thread> thread;
};

// --------------------------------------------------------
// Event related descriptors
// --------------------------------------------------------

class EventHubDescriptor {
public:
	EventHubDescriptor(KernelSharedPtr<EventHub> &&event_hub);

	KernelUnsafePtr<EventHub> getEventHub();

private:
	KernelSharedPtr<EventHub> p_eventHub;
};

// --------------------------------------------------------
// IPC related descriptors
// --------------------------------------------------------

class EndpointDescriptor {
public:
	EndpointDescriptor(KernelSharedPtr<Endpoint> &&endpoint);
	
	KernelUnsafePtr<Endpoint> getEndpoint();

private:
	KernelSharedPtr<Endpoint> p_endpoint;
};

class ServerDescriptor {
public:
	ServerDescriptor(KernelSharedPtr<Server> &&server);
	
	KernelUnsafePtr<Server> getServer();

private:
	KernelSharedPtr<Server> p_server;
};

class ClientDescriptor {
public:
	ClientDescriptor(KernelSharedPtr<Server> &&server);
	
	KernelUnsafePtr<Server> getServer();

private:
	KernelSharedPtr<Server> p_server;
};

// --------------------------------------------------------
// Resource directory related descriptors
// --------------------------------------------------------

class RdDescriptor {
public:
	RdDescriptor(KernelSharedPtr<RdFolder> &&folder);
	
	KernelUnsafePtr<RdFolder> getFolder();

private:
	KernelSharedPtr<RdFolder> p_folder;
};

// --------------------------------------------------------
// IO related descriptors
// --------------------------------------------------------

class IrqDescriptor {
public:
	IrqDescriptor(KernelSharedPtr<IrqLine> &&irq_line);
	
	KernelUnsafePtr<IrqLine> getIrqLine();

private:
	KernelSharedPtr<IrqLine> p_irqLine;
};

class IoDescriptor {
public:
	IoDescriptor(KernelSharedPtr<IoSpace> &&io_space);
	
	KernelUnsafePtr<IoSpace> getIoSpace();

private:
	KernelSharedPtr<IoSpace> p_ioSpace;
};

// --------------------------------------------------------
// AnyDescriptor
// --------------------------------------------------------

typedef frigg::Variant<MemoryAccessDescriptor,
		AddressSpaceDescriptor,
		ThreadDescriptor,
		EventHubDescriptor,
		EndpointDescriptor,
		ServerDescriptor,
		ClientDescriptor,
		RdDescriptor,
		IrqDescriptor,
		IoDescriptor> AnyDescriptor;

} // namespace thor

