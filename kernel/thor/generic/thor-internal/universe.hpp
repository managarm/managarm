#pragma once

#include <expected>
#include <optional>
#include <utility>
#include <frg/variant.hpp>
#include <assert.h>
#include <smarter.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/virtualization.hpp>

namespace thor {

typedef int64_t Handle;

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
struct Credentials;
struct DmaSpace;

struct QueueDescriptor {
	QueueDescriptor(smarter::shared_ptr<IpcQueue> queue)
	: queue(std::move(queue)) { }

	smarter::shared_ptr<IpcQueue> queue;
};

struct UniverseDescriptor {
	UniverseDescriptor(smarter::shared_ptr<Universe> universe)
	: universe(std::move(universe)) { }

	smarter::shared_ptr<Universe> universe;
};

// --------------------------------------------------------
// Memory related descriptors
// --------------------------------------------------------

struct MemoryViewDescriptor {
	MemoryViewDescriptor(smarter::shared_ptr<MemoryView> memory)
	: memory(std::move(memory)) { }

	smarter::shared_ptr<MemoryView> memory;
};

struct MemorySliceDescriptor {
	MemorySliceDescriptor(smarter::shared_ptr<MemorySlice> slice)
	: slice(std::move(slice)) { }

	smarter::shared_ptr<MemorySlice> slice;
};

struct AddressSpaceDescriptor {
	AddressSpaceDescriptor(smarter::shared_ptr<AddressSpace, BindableHandle> space)
	: space(std::move(space)) { }

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
};

struct MemoryViewLockDescriptor {
	MemoryViewLockDescriptor(smarter::shared_ptr<NamedMemoryViewLock> lock)
	: lock(std::move(lock)) { }

	smarter::shared_ptr<NamedMemoryViewLock> lock;
};

struct VirtualizedSpaceDescriptor {
	VirtualizedSpaceDescriptor(smarter::shared_ptr<VirtualizedPageSpace> space)
	: space(std::move(space)) { }

	smarter::shared_ptr<VirtualizedPageSpace> space;
};

struct DmaSpaceDescriptor {
	DmaSpaceDescriptor(smarter::shared_ptr<DmaSpace> space)
	: space(std::move(space)) { }

	smarter::shared_ptr<DmaSpace> space;
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
	ThreadDescriptor(smarter::shared_ptr<Thread, ActiveHandle> thread)
	: thread(std::move(thread)) { }

	smarter::shared_ptr<Thread, ActiveHandle> thread;
};

// --------------------------------------------------------
// IPC related descriptors
// --------------------------------------------------------

struct StreamControl;
struct Stream;

// Refcount policy for smarter::shared_ptr.
// A lane handle is a Stream pointer plus a lane index.
// The refcount that it manipulates is the lane's peer counter.
struct LanePolicy {
	LanePolicy() = default;

	LanePolicy(Stream *stream, int lane)
	: stream_{stream}, lane_{lane} { }

	explicit operator bool () const {
		return stream_;
	}

	void increment() const;
	void decrement() const;

	Stream *stream() const {
		return stream_;
	}

	int lane() const {
		return lane_;
	}

private:
	Stream *stream_ = nullptr;
	int lane_ = -1;
};
static_assert(smarter::rc_policy<LanePolicy>);

// Constructs a lane handle that adopts an existing peer reference on the stream.
inline smarter::shared_ptr<Stream, LanePolicy> adoptLane(
		smarter::borrowed_ptr<Stream> stream, int lane) {
	return smarter::shared_ptr<Stream, LanePolicy>{
			smarter::adopt_rc, stream.get(), LanePolicy{stream.get(), lane}};
}

// Extracts the numeric lane index of a lane handle.
inline int laneOf(const smarter::shared_ptr<Stream, LanePolicy> &lane) {
	return lane.policy().lane();
}

struct LaneDescriptor {
	LaneDescriptor() = default;

	explicit LaneDescriptor(smarter::shared_ptr<Stream, LanePolicy> handle)
	: handle(std::move(handle)) { }

	smarter::shared_ptr<Stream, LanePolicy> handle;
};

// --------------------------------------------------------
// Event related descriptors.
// --------------------------------------------------------

struct IrqObject;
struct OneshotEvent;
struct BitsetEvent;

struct OneshotEventDescriptor {
	OneshotEventDescriptor(smarter::shared_ptr<OneshotEvent> event)
	: event{std::move(event)} { }

	smarter::shared_ptr<OneshotEvent> event;
};

struct BitsetEventDescriptor {
	BitsetEventDescriptor(smarter::shared_ptr<BitsetEvent> event)
	: event{std::move(event)} { }

	smarter::shared_ptr<BitsetEvent> event;
};

struct IrqDescriptor {
	IrqDescriptor(smarter::shared_ptr<IrqObject> irq)
	: irq{std::move(irq)} { }

	smarter::shared_ptr<IrqObject> irq;
};

// --------------------------------------------------------
// I/O related descriptors.
// --------------------------------------------------------

struct IoDescriptor {
	IoDescriptor(smarter::shared_ptr<IoSpace> io_space)
	: ioSpace(std::move(io_space)) { }

	smarter::shared_ptr<IoSpace> ioSpace;
};

// --------------------------------------------------------
// Kernlet related descriptors.
// --------------------------------------------------------

struct KernletObjectDescriptor {
	KernletObjectDescriptor(smarter::shared_ptr<KernletObject> kernlet_object)
	: kernletObject(std::move(kernlet_object)) { }

	smarter::shared_ptr<KernletObject> kernletObject;
};

struct BoundKernletDescriptor {
	BoundKernletDescriptor(smarter::shared_ptr<BoundKernlet> bound_kernlet)
	: boundKernlet(std::move(bound_kernlet)) { }

	smarter::shared_ptr<BoundKernlet> boundKernlet;
};

// --------------------------------------------------------
// Token related descriptors.
// --------------------------------------------------------

struct TokenDescriptor {
	TokenDescriptor(smarter::shared_ptr<Credentials> credentials)
	: credentials(std::move(credentials)) { }

	smarter::shared_ptr<Credentials> credentials;
};

// --------------------------------------------------------
// AnyDescriptor
// --------------------------------------------------------

typedef frg::variant<
	UniverseDescriptor,
	QueueDescriptor,
	MemoryViewDescriptor,
	MemorySliceDescriptor,
	AddressSpaceDescriptor,
	VirtualizedSpaceDescriptor,
	DmaSpaceDescriptor,
	VirtualizedCpuDescriptor,
	MemoryViewLockDescriptor,
	ThreadDescriptor,
	LaneDescriptor,
	IrqDescriptor,
	OneshotEventDescriptor,
	BitsetEventDescriptor,
	IoDescriptor,
	KernletObjectDescriptor,
	BoundKernletDescriptor,
	TokenDescriptor
> AnyDescriptor;

// --------------------------------------------------------
// Universe.
// --------------------------------------------------------

struct Universe {
public:
	typedef frg::ticket_spinlock Lock;
	typedef frg::unique_lock<frg::ticket_spinlock> Guard;

	Universe();
	~Universe();

	Handle attachDescriptor(AnyDescriptor descriptor);

	std::optional<AnyDescriptor> getDescriptor(Handle handle);

	template<typename Fn>
	requires requires(Fn fn, AnyDescriptor &desc) {
		{ fn(desc) };
	}
	auto inspectDescriptor(Handle handle, Fn &&fn)
			-> std::invoke_result_t<Fn, AnyDescriptor &> {
		using ResultType = std::invoke_result_t<Fn, AnyDescriptor &>;

		auto irqLock = frg::guard(&irqMutex());
		Guard guard(lock);

		auto *desc = _descriptorMap.get(handle);
		if(!desc)
			return ResultType{std::unexpect, Error::noDescriptor};
		return std::forward<Fn>(fn)(*desc);
	}

	frg::optional<AnyDescriptor> detachDescriptor(Handle handle);

	Lock lock;

private:
	frg::hash_map<
		Handle,
		AnyDescriptor,
		frg::hash<Handle>,
		KernelAlloc
	> _descriptorMap;

	Handle _nextHandle;
};

} // namespace thor
