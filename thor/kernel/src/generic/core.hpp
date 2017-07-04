#ifndef THOR_GENERIC_CORE_HPP
#define THOR_GENERIC_CORE_HPP

#include <frigg/callback.hpp>
#include <frigg/variant.hpp>
#include <frg/pairing_heap.hpp>
#include "error.hpp"
#include "../arch/x86/cpu.hpp"

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
// Kernel data types
// --------------------------------------------------------

typedef int64_t Handle;

class Universe;
class Memory;
class AddressSpace;
class Thread;
class Stream;
class LaneControl;
class IoSpace;

struct KernelFiber;

struct CpuData : public PlatformCpuData {
	CpuData();

	IrqMutex irqMutex;
	KernelFiber *activeFiber;
};

struct Timer {
	Timer(uint64_t deadline, frigg::CallbackPtr<void()> callback)
	: deadline{deadline}, callback{callback} { }

	bool operator< (const Timer &other) {
		return deadline < other.deadline;
	}

	uint64_t deadline;

	frigg::CallbackPtr<void()> callback;

	frg::pairing_heap_hook<Timer> hook;
};

struct SubmitInfo {
	SubmitInfo();

	SubmitInfo(int64_t async_id, uintptr_t submit_function,
			uintptr_t submit_object);
	
	int64_t asyncId;
	uintptr_t submitFunction;
	uintptr_t submitObject;
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
DirectSpaceAccessor<T>::DirectSpaceAccessor(ForeignSpaceAccessor &lock, ptrdiff_t offset) {
	static_assert(!(kPageSize % sizeof(T)), "Type too large for DirectSpaceAccessor");
	assert(!(lock.address() % sizeof(T)));
	
	_misalign = (lock.address() + offset) % kPageSize;
	AddressSpace::Guard guard(&lock.space()->lock);
	auto physical = lock.space()->grabPhysical(guard, lock.address() + offset - _misalign);
	_accessor = PageAccessor{generalWindow, physical};
}

inline void ForeignSpaceAccessor::load(size_t offset, void *pointer, size_t size) {
	AddressSpace::Guard guard(&_space->lock);
	
	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = (VirtualAddr)_address + offset + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = _space->grabPhysical(guard, write - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{generalWindow, page};
		memcpy((char *)pointer + progress, (char *)accessor.get() + misalign, chunk);
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
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{generalWindow, page};
		memcpy((char *)accessor.get() + misalign, (char *)pointer + progress, chunk);
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

#endif // THOR_GENERIC_CORE_HPP
