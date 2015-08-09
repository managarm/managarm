
namespace thor {

// --------------------------------------------------------
// Memory related descriptors
// --------------------------------------------------------

class MemoryAccessDescriptor {
public:
	MemoryAccessDescriptor(SharedPtr<Memory, KernelAlloc> &&memory);

	UnsafePtr<Memory, KernelAlloc> getMemory();

private:
	SharedPtr<Memory, KernelAlloc> p_memory;
};

class AddressSpaceDescriptor {
public:
	AddressSpaceDescriptor(SharedPtr<AddressSpace, KernelAlloc> &&space);

	UnsafePtr<AddressSpace, KernelAlloc> getSpace();

private:
	SharedPtr<AddressSpace, KernelAlloc> p_space;
};

// --------------------------------------------------------
// Threading related descriptors
// --------------------------------------------------------

class ThreadObserveDescriptor {
public:
	ThreadObserveDescriptor(SharedPtr<Thread, KernelAlloc> &&thread);
	
	UnsafePtr<Thread, KernelAlloc> getThread();

private:
	SharedPtr<Thread, KernelAlloc> p_thread;
};

// --------------------------------------------------------
// Event related descriptors
// --------------------------------------------------------

class EventHubDescriptor {
public:
	EventHubDescriptor(SharedPtr<EventHub, KernelAlloc> &&event_hub);

	UnsafePtr<EventHub, KernelAlloc> getEventHub();

private:
	SharedPtr<EventHub, KernelAlloc> p_eventHub;
};

// --------------------------------------------------------
// IPC related descriptors
// --------------------------------------------------------

// Reads from the first channel, writes to the second
class BiDirectionFirstDescriptor {
public:
	BiDirectionFirstDescriptor(SharedPtr<BiDirectionPipe, KernelAlloc> &&pipe);
	
	UnsafePtr<BiDirectionPipe, KernelAlloc> getPipe();

private:
	SharedPtr<BiDirectionPipe, KernelAlloc> p_pipe;
};

// Reads from the second channel, writes to the first
class BiDirectionSecondDescriptor {
public:
	BiDirectionSecondDescriptor(SharedPtr<BiDirectionPipe, KernelAlloc> &&pipe);
	
	UnsafePtr<BiDirectionPipe, KernelAlloc> getPipe();

private:
	SharedPtr<BiDirectionPipe, KernelAlloc> p_pipe;
};

class ServerDescriptor {
public:
	ServerDescriptor(SharedPtr<Server, KernelAlloc> &&server);
	
	UnsafePtr<Server, KernelAlloc> getServer();

private:
	SharedPtr<Server, KernelAlloc> p_server;
};

class ClientDescriptor {
public:
	ClientDescriptor(SharedPtr<Server, KernelAlloc> &&server);
	
	UnsafePtr<Server, KernelAlloc> getServer();

private:
	SharedPtr<Server, KernelAlloc> p_server;
};

// --------------------------------------------------------
// Resource directory related descriptors
// --------------------------------------------------------

class RdDescriptor {
public:
	RdDescriptor(SharedPtr<RdFolder, KernelAlloc> &&folder);
	
	UnsafePtr<RdFolder, KernelAlloc> getFolder();

private:
	SharedPtr<RdFolder, KernelAlloc> p_folder;
};

// --------------------------------------------------------
// IO related descriptors
// --------------------------------------------------------

class IrqDescriptor {
public:
	IrqDescriptor(SharedPtr<IrqLine, KernelAlloc> &&irq_line);
	
	UnsafePtr<IrqLine, KernelAlloc> getIrqLine();

private:
	SharedPtr<IrqLine, KernelAlloc> p_irqLine;
};

class IoDescriptor {
public:
	IoDescriptor(SharedPtr<IoSpace, KernelAlloc> &&io_space);
	
	UnsafePtr<IoSpace, KernelAlloc> getIoSpace();

private:
	SharedPtr<IoSpace, KernelAlloc> p_ioSpace;
};

// --------------------------------------------------------
// AnyDescriptor
// --------------------------------------------------------

typedef frigg::util::Variant<MemoryAccessDescriptor,
		AddressSpaceDescriptor,
		ThreadObserveDescriptor,
		EventHubDescriptor,
		BiDirectionFirstDescriptor,
		BiDirectionSecondDescriptor,
		ServerDescriptor,
		ClientDescriptor,
		RdDescriptor,
		IrqDescriptor,
		IoDescriptor> AnyDescriptor;

} // namespace thor

