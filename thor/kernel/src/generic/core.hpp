
namespace thor {

int64_t allocAsyncId();

// --------------------------------------------------------
// Debugging and logging
// --------------------------------------------------------

class BochsSink {
public:
	void print(char c);
	void print(const char *str);
};

extern BochsSink infoSink;

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

class KernelVirtualAlloc {
public:
	KernelVirtualAlloc();

	uintptr_t map(size_t length);
	void unmap(uintptr_t address, size_t length);
};

typedef frigg::SlabAllocator<KernelVirtualAlloc, frigg::TicketLock> KernelAlloc;
extern frigg::LazyInitializer<PhysicalChunkAllocator> physicalAllocator;
extern frigg::LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;
extern frigg::LazyInitializer<KernelAlloc> kernelAlloc;

template<typename T>
using KernelSharedPtr = frigg::SharedPtr<T>;

template<typename T>
using KernelWeakPtr = frigg::WeakPtr<T>;

template<typename T>
using KernelUnsafePtr = frigg::UnsafePtr<T>;

// --------------------------------------------------------
// Kernel data types
// --------------------------------------------------------

enum Error {
	kErrSuccess,
	kErrBufferTooSmall,
	kErrClosedLocally,
	kErrClosedRemotely
};

typedef int64_t Handle;

class Universe;
class Memory;
class AddressSpace;
class Thread;
class EventHub;
class Stream;
class LaneControl;
class RingBuffer;
class Channel;
class BiDirectionPipe;
class Endpoint;
class IrqLine;
class IoSpace;

struct Context : public PlatformContext {
	Context(void *kernel_stack_base)
	: PlatformContext(kernel_stack_base) { };

	Context(const Context &other) = delete;
	Context(Context &&other) = delete;
	Context &operator= (Context context) = delete;
};

struct CpuData : public PlatformCpuData {
	CpuData();

	Context context;
};

struct Timer {
	Timer(uint64_t deadline)
	: deadline(deadline) { }

	bool operator< (const Timer &other) {
		return deadline < other.deadline;
	}

	uint64_t deadline;

	KernelWeakPtr<Thread> thread;
};

struct SubmitInfo {
	SubmitInfo();

	SubmitInfo(int64_t async_id, uintptr_t submit_function,
			uintptr_t submit_object);
	
	int64_t asyncId;
	uintptr_t submitFunction;
	uintptr_t submitObject;
};

struct NullCompleter {

};

struct PostEventCompleter {
	PostEventCompleter(frigg::WeakPtr<EventHub> event_hub, int64_t async_id,
			uintptr_t submit_function, uintptr_t submit_object)
	: eventHub(frigg::move(event_hub)),
			submitInfo(async_id, submit_function, submit_object) { }
	
	frigg::WeakPtr<EventHub> eventHub;
	SubmitInfo submitInfo;
};

struct ReturnFromForkCompleter {
	ReturnFromForkCompleter(frigg::WeakPtr<Thread> thread)
	: thread(frigg::move(thread)) { }

	frigg::WeakPtr<Thread> thread;
};

// this is a base class for async request classes
struct BaseRequest {
	BaseRequest(KernelSharedPtr<EventHub> event_hub, SubmitInfo submit_info);
	
	KernelSharedPtr<EventHub> eventHub;
	SubmitInfo submitInfo;
};

struct ThreadRunControl {
	ThreadRunControl()
	: _counter(nullptr) { }

	ThreadRunControl(Thread *thread, frigg::SharedCounter *counter)
	: _thread(thread), _counter(counter) { }

	explicit operator bool () {
		return _counter;
	}

	operator frigg::SharedControl () const {
		return frigg::SharedControl(_counter);
	}

	void increment();
	void decrement();

private:
	Thread *_thread;
	frigg::SharedCounter *_counter;
};

struct EndpointRwControl {
	EndpointRwControl()
	: _counter(nullptr) { }

	EndpointRwControl(Endpoint *endpoint, frigg::SharedCounter *counter)
	: _endpoint(endpoint), _counter(counter) { }

	explicit operator bool () {
		return _counter;
	}

	operator frigg::SharedControl () const {
		return frigg::SharedControl(_counter);
	}

	void increment();
	void decrement();

private:
	Endpoint *_endpoint;
	frigg::SharedCounter *_counter;
};

} // namespace thor

#include "descriptor.hpp"
#include "accessors.hpp"
#include "event.hpp"
#include "usermem.hpp"
#include "thread.hpp"
#include "stream.hpp"
#include "ring-buffer.hpp"
#include "ipc.hpp"
#include "io.hpp"

namespace thor {

template<typename T>
T *DirectSpaceAccessor<T>::get() {
	assert(_space);
	size_t misalign = (VirtualAddr)_address % kPageSize;
	AddressSpace::Guard guard(&_space->lock);
	PhysicalAddr page = _space->grabPhysical(guard, (VirtualAddr)_address - misalign);
	return reinterpret_cast<T *>(physicalToVirtual(page + misalign));
}

inline void ForeignSpaceAccessor::copyTo(void *pointer, size_t size) {
	AddressSpace::Guard guard(&_space->lock);
	
	size_t offset = 0;
	while(offset < size) {
		VirtualAddr write = (VirtualAddr)_address + offset;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frigg::min(kPageSize - misalign, size - offset);

		PhysicalAddr page = _space->grabPhysical(guard, write - misalign);
		memcpy(physicalToVirtual(page + misalign), (char *)pointer + offset, chunk);
		offset += chunk;
	}
}

// --------------------------------------------------------
// Process related classes
// --------------------------------------------------------

class Universe {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<frigg::TicketLock> Guard;

	Universe();

	// these channels can be used to send/receive packets to/from
	// an inferior/superior universe.
	Channel &inferiorSendChannel() { return _channels[0]; }
	Channel &inferiorRecvChannel() { return _channels[1]; }

	Channel &superiorSendChannel() { return _channels[1]; }
	Channel &superiorRecvChannel() { return _channels[0]; }
	
	Handle attachDescriptor(Guard &guard, AnyDescriptor descriptor);

	AnyDescriptor *getDescriptor(Guard &guard, Handle handle);
	
	frigg::Optional<AnyDescriptor> detachDescriptor(Guard &guard, Handle handle);

	Lock lock;

private:
	Channel _channels[2];

	frigg::Hashmap<Handle, AnyDescriptor,
			frigg::DefaultHasher<Handle>, KernelAlloc> _descriptorMap;
	Handle _nextHandle;
};

} // namespace thor

