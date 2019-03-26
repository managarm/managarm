#ifndef THOR_GENERIC_DESCRIPTOR_HPP
#define THOR_GENERIC_DESCRIPTOR_HPP

#include <frigg/smart_ptr.hpp>
#include <frigg/variant.hpp>

namespace thor {

struct AddressSpace;
struct IoSpace;
struct Memory;
struct Thread;
struct Universe;
struct IpcQueue;
struct MemorySlice;
struct KernletObject;
struct BoundKernlet;

struct QueueDescriptor {
	QueueDescriptor(frigg::SharedPtr<IpcQueue> queue)
	: queue(frigg::move(queue)) { }

	frigg::SharedPtr<IpcQueue> queue;
};

struct UniverseDescriptor {
	UniverseDescriptor(frigg::SharedPtr<Universe> universe)
	: universe(frigg::move(universe)) { }

	frigg::SharedPtr<Universe> universe;
};

// --------------------------------------------------------
// Memory related descriptors
// --------------------------------------------------------

struct MemoryViewDescriptor {
	MemoryViewDescriptor(frigg::SharedPtr<Memory> memory)
	: memory(frigg::move(memory)) { }

	frigg::SharedPtr<Memory> memory;
};

struct MemorySliceDescriptor {
	MemorySliceDescriptor(frigg::SharedPtr<MemorySlice> slice)
	: slice(frigg::move(slice)) { }

	frigg::SharedPtr<MemorySlice> slice;
};

struct AddressSpaceDescriptor {
	AddressSpaceDescriptor(frigg::SharedPtr<AddressSpace> space)
	: space(frigg::move(space)) { }

	frigg::SharedPtr<AddressSpace> space;
};

// --------------------------------------------------------
// Threading related descriptors
// --------------------------------------------------------

struct ThreadDescriptor {
	ThreadDescriptor(frigg::SharedPtr<Thread> thread)
	: thread(frigg::move(thread)) { }

	frigg::SharedPtr<Thread> thread;
};

// --------------------------------------------------------
// IPC related descriptors
// --------------------------------------------------------

struct StreamControl;
struct Stream;

struct AdoptLane { };
static constexpr AdoptLane adoptLane;

// TODO: implement SharedLaneHandle + UnsafeLaneHandle?
struct LaneHandle {
	friend void swap(LaneHandle &a, LaneHandle &b) {
		using frigg::swap;
		swap(a._stream, b._stream);
		swap(a._lane, b._lane);
	}

	// Initialize _lane so that the compiler does not complain about uninitialized values.
	LaneHandle()
	: _lane{-1} { };

	explicit LaneHandle(AdoptLane, frigg::UnsafePtr<Stream> stream, int lane)
	: _stream(stream), _lane(lane) { }

	LaneHandle(const LaneHandle &other);

	LaneHandle(LaneHandle &&other)
	: LaneHandle() {
		swap(*this, other);
	}

	~LaneHandle();

	explicit operator bool () {
		return static_cast<bool>(_stream);
	}

	LaneHandle &operator= (LaneHandle other) {
		swap(*this, other);
		return *this;
	}

	frigg::UnsafePtr<Stream> getStream() {
		return _stream;
	}

	int getLane() {
		return _lane;
	}

private:
	frigg::UnsafePtr<Stream> _stream;
	int _lane;
};

struct LaneDescriptor {
	LaneDescriptor() = default;

	explicit LaneDescriptor(LaneHandle handle)
	: handle(frigg::move(handle)) { }

	LaneHandle handle;
};

// --------------------------------------------------------
// Event related descriptors.
// --------------------------------------------------------

struct IrqObject;
struct OneshotEvent;
struct BitsetEvent;

struct OneshotEventDescriptor {
	OneshotEventDescriptor(frigg::SharedPtr<OneshotEvent> event)
	: event{frigg::move(event)} { }

	frigg::SharedPtr<OneshotEvent> event;
};

struct BitsetEventDescriptor {
	BitsetEventDescriptor(frigg::SharedPtr<BitsetEvent> event)
	: event{frigg::move(event)} { }

	frigg::SharedPtr<BitsetEvent> event;
};

struct IrqDescriptor {
	IrqDescriptor(frigg::SharedPtr<IrqObject> irq)
	: irq{frigg::move(irq)} { }

	frigg::SharedPtr<IrqObject> irq;
};

// --------------------------------------------------------
// I/O related descriptors.
// --------------------------------------------------------

struct IoDescriptor {
	IoDescriptor(frigg::SharedPtr<IoSpace> io_space)
	: ioSpace(frigg::move(io_space)) { }

	frigg::SharedPtr<IoSpace> ioSpace;
};

// --------------------------------------------------------
// AnyDescriptor
// --------------------------------------------------------

struct KernletObjectDescriptor {
	KernletObjectDescriptor(frigg::SharedPtr<KernletObject> kernlet_object)
	: kernletObject(frigg::move(kernlet_object)) { }

	frigg::SharedPtr<KernletObject> kernletObject;
};

struct BoundKernletDescriptor {
	BoundKernletDescriptor(frigg::SharedPtr<BoundKernlet> bound_kernlet)
	: boundKernlet(frigg::move(bound_kernlet)) { }

	frigg::SharedPtr<BoundKernlet> boundKernlet;
};

typedef frigg::Variant<
	UniverseDescriptor,
	QueueDescriptor,
	MemoryViewDescriptor,
	MemorySliceDescriptor,
	AddressSpaceDescriptor,
	ThreadDescriptor,
	LaneDescriptor,
	IrqDescriptor,
	OneshotEventDescriptor,
	BitsetEventDescriptor,
	IoDescriptor,
	KernletObjectDescriptor,
	BoundKernletDescriptor
> AnyDescriptor;

} // namespace thor

#endif // THOR_GENERIC_DESCRIPTOR_HPP
