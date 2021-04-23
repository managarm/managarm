#pragma once

#include <frg/hash_map.hpp>
#include <frg/optional.hpp>
#include <frg/variant.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/executor-context.hpp>
#include <thor-internal/ring-buffer.hpp>
#include <thor-internal/schedule.hpp>

namespace thor {

// --------------------------------------------------------
// Kernel data types
// --------------------------------------------------------

typedef int64_t Handle;

struct Universe;
struct Memory;
struct AddressSpace;
struct Thread;
struct Stream;
struct LaneControl;
struct IoSpace;

struct WorkQueue;
struct KernelFiber;

enum class ProfileMechanism {
	none,
	intelPmc,
	amdPmc
};

struct CpuData : public PlatformCpuData {
	CpuData();

	CpuData(const CpuData &) = delete;

	CpuData &operator= (const CpuData &) = delete;

	IrqMutex irqMutex;
	Scheduler scheduler;
	bool haveVirtualization;

	int cpuIndex;

	ExecutorContext *executorContext = nullptr;
	KernelFiber *activeFiber;
	KernelFiber *wqFiber = nullptr;
	smarter::shared_ptr<WorkQueue> generalWorkQueue;
	std::atomic<uint64_t> heartbeat;

	unsigned int irqEntropySeq = 0;
	std::atomic<ProfileMechanism> profileMechanism{};
	// TODO: This should be a unique_ptr instead.
	SingleContextRecordRing *localProfileRing = nullptr;
};

CpuData *getCpuData(size_t k);
int getCpuCount();
inline CpuData *getCpuData() {
	return static_cast<CpuData *>(getPlatformCpuData());
}

inline ExecutorContext *currentExecutorContext() {
	return getCpuData()->executorContext;
}

} // namespace thor

#include <thor-internal/accessors.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/descriptor.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

template<typename T>
DirectSpaceAccessor<T>::DirectSpaceAccessor(AddressSpaceLockHandle &lock, ptrdiff_t offset) {
	static_assert(sizeof(T) < kPageSize, "Type too large for DirectSpaceAccessor");
	assert(!(lock.address() % sizeof(T)));
	
	_misalign = (lock.address() + offset) & (kPageSize - 1);
	auto physical = lock.getPhysical(offset);
	assert(physical != PhysicalAddr(-1));
	_accessor = PageAccessor{physical};
}

// --------------------------------------------------------
// Process related classes
// --------------------------------------------------------

struct Universe {
public:
	typedef frg::ticket_spinlock Lock;
	typedef frg::unique_lock<frg::ticket_spinlock> Guard;

	Universe();
	~Universe();

	Handle attachDescriptor(Guard &guard, AnyDescriptor descriptor);

	AnyDescriptor *getDescriptor(Guard &guard, Handle handle);
	
	frg::optional<AnyDescriptor> detachDescriptor(Guard &guard, Handle handle);

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
