#pragma once

#include <frigg/smart_ptr.hpp>
#include <frg/variant.hpp>
#include <smarter.hpp>
#ifdef __x86_64__
#include <thor-internal/arch/ept.hpp>
#include <thor-internal/arch/vmx.hpp>
#endif
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/virtualization.hpp>

namespace thor {

struct MemoryView;
struct AddressSpace;
struct IoSpace;
struct Thread;
struct Universe;
struct IpcQueue;
struct MemorySlice;
struct NamedMemoryViewLock;
struct KernletObject;
struct BoundKernlet;

struct QueueDescriptor {
	QueueDescriptor(frigg::SharedPtr<IpcQueue> queue)
	: queue(std::move(queue)) { }

	frigg::SharedPtr<IpcQueue> queue;
};

struct UniverseDescriptor {
	UniverseDescriptor(frigg::SharedPtr<Universe> universe)
	: universe(std::move(universe)) { }

	frigg::SharedPtr<Universe> universe;
};

// --------------------------------------------------------
// Memory related descriptors
// --------------------------------------------------------

struct MemoryViewDescriptor {
	MemoryViewDescriptor(frigg::SharedPtr<MemoryView> memory)
	: memory(std::move(memory)) { }

	frigg::SharedPtr<MemoryView> memory;
};

struct MemorySliceDescriptor {
	MemorySliceDescriptor(frigg::SharedPtr<MemorySlice> slice)
	: slice(std::move(slice)) { }

	frigg::SharedPtr<MemorySlice> slice;
};

struct AddressSpaceDescriptor {
	AddressSpaceDescriptor(smarter::shared_ptr<AddressSpace, BindableHandle> space)
	: space(std::move(space)) { }

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
};

struct MemoryViewLockDescriptor {
	MemoryViewLockDescriptor(frigg::SharedPtr<NamedMemoryViewLock> lock)
	: lock(std::move(lock)) { }

	frigg::SharedPtr<NamedMemoryViewLock> lock;
};

struct VirtualizedSpaceDescriptor {
	VirtualizedSpaceDescriptor(smarter::shared_ptr<VirtualizedPageSpace> space)
	: space(std::move(space)) { }

	smarter::shared_ptr<VirtualizedPageSpace> space;
};

struct VirtualizedCpuDescriptor {
	VirtualizedCpuDescriptor() : vcpu(nullptr) { }
	VirtualizedCpuDescriptor(smarter::shared_ptr<VirtualizedCpu> vcpu)
	: vcpu(std::move(vcpu)) { }

	smarter::shared_ptr<VirtualizedCpu> vcpu;
};

// --------------------------------------------------------
// Threading related descriptors
// --------------------------------------------------------

struct ThreadDescriptor {
	ThreadDescriptor(frigg::SharedPtr<Thread> thread)
	: thread(std::move(thread)) { }

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
		using std::swap;
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
	: handle(std::move(handle)) { }

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
	: event{std::move(event)} { }

	frigg::SharedPtr<OneshotEvent> event;
};

struct BitsetEventDescriptor {
	BitsetEventDescriptor(frigg::SharedPtr<BitsetEvent> event)
	: event{std::move(event)} { }

	frigg::SharedPtr<BitsetEvent> event;
};

struct IrqDescriptor {
	IrqDescriptor(frigg::SharedPtr<IrqObject> irq)
	: irq{std::move(irq)} { }

	frigg::SharedPtr<IrqObject> irq;
};

// --------------------------------------------------------
// I/O related descriptors.
// --------------------------------------------------------

struct IoDescriptor {
	IoDescriptor(frigg::SharedPtr<IoSpace> io_space)
	: ioSpace(std::move(io_space)) { }

	frigg::SharedPtr<IoSpace> ioSpace;
};

// --------------------------------------------------------
// AnyDescriptor
// --------------------------------------------------------

struct KernletObjectDescriptor {
	KernletObjectDescriptor(frigg::SharedPtr<KernletObject> kernlet_object)
	: kernletObject(std::move(kernlet_object)) { }

	frigg::SharedPtr<KernletObject> kernletObject;
};

struct BoundKernletDescriptor {
	BoundKernletDescriptor(frigg::SharedPtr<BoundKernlet> bound_kernlet)
	: boundKernlet(std::move(bound_kernlet)) { }

	frigg::SharedPtr<BoundKernlet> boundKernlet;
};

typedef frg::variant<
	UniverseDescriptor,
	QueueDescriptor,
	MemoryViewDescriptor,
	MemorySliceDescriptor,
	AddressSpaceDescriptor,
	VirtualizedSpaceDescriptor,
	VirtualizedCpuDescriptor,
	MemoryViewLockDescriptor,
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
