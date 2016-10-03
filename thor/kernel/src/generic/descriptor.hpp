
namespace thor {

// --------------------------------------------------------
// Memory related descriptors
// --------------------------------------------------------

struct MemoryAccessDescriptor {
	MemoryAccessDescriptor(frigg::SharedPtr<Memory> memory)
	: memory(frigg::move(memory)) { }

	frigg::SharedPtr<Memory> memory;
};

struct AddressSpaceDescriptor {
	AddressSpaceDescriptor(frigg::SharedPtr<AddressSpace> space)
	: space(frigg::move(space)) { }

	frigg::SharedPtr<AddressSpace> space;
};

// --------------------------------------------------------
// Threading related descriptors
// --------------------------------------------------------

struct UniverseDescriptor {
	UniverseDescriptor(frigg::SharedPtr<Universe> universe)
	: universe(frigg::move(universe)) { }

	frigg::SharedPtr<Universe> universe;
};

struct ThreadDescriptor {
	ThreadDescriptor(frigg::SharedPtr<Thread, ThreadRunControl> thread)
	: thread(frigg::move(thread)) { }
	
	frigg::SharedPtr<Thread, ThreadRunControl> thread;
};

// --------------------------------------------------------
// Event related descriptors
// --------------------------------------------------------

struct EventHubDescriptor {
	EventHubDescriptor(frigg::SharedPtr<EventHub> event_hub)
	: eventHub(frigg::move(event_hub)) { }

	frigg::SharedPtr<EventHub> eventHub;
};

// --------------------------------------------------------
// IPC related descriptors
// --------------------------------------------------------

struct RingDescriptor {
	RingDescriptor(frigg::SharedPtr<RingBuffer> ring_buffer)
	: ringBuffer(frigg::move(ring_buffer)) { }
	
	frigg::SharedPtr<RingBuffer> ringBuffer;
};

struct EndpointDescriptor {
	EndpointDescriptor(frigg::SharedPtr<Endpoint, EndpointRwControl> endpoint)
	: endpoint(frigg::move(endpoint)) { }
	
	frigg::SharedPtr<Endpoint, EndpointRwControl> endpoint;
};

// --------------------------------------------------------
// IO related descriptors
// --------------------------------------------------------

struct IrqDescriptor {
	IrqDescriptor(frigg::SharedPtr<IrqLine> irq_line)
	: irqLine(frigg::move(irq_line)) { }
	
	frigg::SharedPtr<IrqLine> irqLine;
};

struct IoDescriptor {
	IoDescriptor(frigg::SharedPtr<IoSpace> io_space)
	: ioSpace(frigg::move(io_space)) { }
	
	frigg::SharedPtr<IoSpace> ioSpace;
};

// --------------------------------------------------------
// AnyDescriptor
// --------------------------------------------------------

typedef frigg::Variant<
	MemoryAccessDescriptor,
	AddressSpaceDescriptor,
	UniverseDescriptor,
	ThreadDescriptor,
	EventHubDescriptor,
	RingDescriptor,
	EndpointDescriptor,
	IrqDescriptor,
	IoDescriptor
> AnyDescriptor;

} // namespace thor

