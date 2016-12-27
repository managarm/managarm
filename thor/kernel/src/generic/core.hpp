
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
class Stream;
class LaneControl;
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


struct ThreadRunControl {
	ThreadRunControl()
	: _counter(nullptr) { }

	ThreadRunControl(Thread *thread, frigg::SharedCounter *counter)
	: _thread(thread), _counter(counter) { }

	explicit operator bool () const {
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

} // namespace thor

#include "descriptor.hpp"
#include "accessors.hpp"
#include "usermem.hpp"
#include "thread.hpp"
#include "stream.hpp"
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

inline void ForeignSpaceAccessor::load(size_t offset, void *pointer, size_t size) {
	AddressSpace::Guard guard(&_space->lock);
	
	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = (VirtualAddr)_address + offset + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = _space->grabPhysical(guard, write - misalign);
		memcpy((char *)pointer + progress, physicalToVirtual(page + misalign), chunk);
		progress += chunk;
	}
}

inline void ForeignSpaceAccessor::copyTo(size_t offset, void *pointer, size_t size) {
	AddressSpace::Guard guard(&_space->lock);
	
	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = (VirtualAddr)_address + offset + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = _space->grabPhysical(guard, write - misalign);
		memcpy(physicalToVirtual(page + misalign), (char *)pointer + progress, chunk);
		progress += chunk;
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

	Handle attachDescriptor(Guard &guard, AnyDescriptor descriptor);

	AnyDescriptor *getDescriptor(Guard &guard, Handle handle);
	
	frigg::Optional<AnyDescriptor> detachDescriptor(Guard &guard, Handle handle);

	Lock lock;

private:
	frigg::Hashmap<Handle, AnyDescriptor,
			frigg::DefaultHasher<Handle>, KernelAlloc> _descriptorMap;
	Handle _nextHandle;
};

} // namespace thor

