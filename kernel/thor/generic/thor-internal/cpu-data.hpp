#pragma once

#include <eir/interface.hpp>
#include <thor-internal/elf-notes.hpp>
#include <thor-internal/arch-generic/cpu-data.hpp>
#include <thor-internal/executor-context.hpp>
#include <thor-internal/kernel-locks.hpp>

#include <new>
#include <tuple>

namespace thor {

extern ManagarmElfNote<CpuConfig> cpuConfigNote;

// Forward defined for pointers that are part of CpuData.
struct Thread;
struct KernelFiber;
struct SingleContextRecordRing;
struct ReentrantRecordRing;
struct SelfIntCallBase;
struct WorkQueue;

enum class ProfileMechanism {
	none,
	intelPmc,
	amdPmc
};

// "Interrupt priority level". This is our version of the IRQL that the NT kernel uses.
// Note that this is a software concept that does *not* correspond to hardware IRQ priorities.
// Code running at IPL L can safely access thread-local data structures
// if these data structures are only ever accessed at IPL <= L.
using Ipl = short;

using IplMask = uint32_t;

namespace ipl {
// Sentinel / invalid value.
inline constexpr Ipl bad = -1;
// Level that threads run at (unless they raise IPL).
inline constexpr Ipl passive = 0;
// Level that page faults run at.
// Accessing lower-half memory is only allowed at currentIpl() < ipl::exceptional.
inline constexpr Ipl exceptional = 1;
// Blocking is only allowed at currentIpl() < ipl::schedule.
// Threads may only be scheduled out if Executor::iplState()->current < ipl::schedule.
inline constexpr Ipl schedule = 2;
// Level that interrupts run at.
// Also, level that the scheduler itself runs at.
inline constexpr Ipl interrupt = 3;
// Level that exceptions and NMIs run at.
// This is the only level that can be entered multiple times
// (i.e., ipl::maximal -> ipl::maximal entries are allowed).
inline constexpr Ipl maximal = 4;
} // namespace ipl

struct alignas(4) IplState {
	// Level of the current context.
	Ipl context{ipl::passive};
	// Level of the currenly executing code path. This is always above the context level.
	Ipl current{ipl::passive};
};
static_assert(std::atomic<IplState>::is_always_lock_free);

struct CpuData : public PlatformCpuData {
	static constexpr unsigned int RS_EMITTING = 1;
	static constexpr unsigned int RS_PENDING = 2;

	CpuData();

	CpuData(const CpuData &) = delete;

	CpuData &operator= (const CpuData &) = delete;

	std::atomic<IplState> iplState;
	std::atomic<uint32_t> iplDeferred{0};
	IrqMutex irqMutex;
	UniqueKernelStack detachedStack;
	UniqueKernelStack idleStack;
	bool haveVirtualization;

	int cpuIndex;

	ExecutorContext *executorContext{nullptr};
	smarter::borrowed_ptr<Thread> activeThread;
	KernelFiber *activeFiber{nullptr};
	KernelFiber *wqFiber{nullptr};
	std::atomic<SelfIntCallBase *> selfIntCallPtr{nullptr};
	smarter::shared_ptr<WorkQueue> generalWorkQueue;
	std::atomic<uint64_t> heartbeat;

	IseqContext regularIseq;

	// Ring buffer that stores log records that are produced on this CPU.
	// This is reentrant, i.e., it allows non-maskable interrupts / exceptions to log data.
	// The ring buffer is drained to the global logging sinks.
	ReentrantRecordRing *localLogRing;
	// Current dequeue sequence for localLogRing.
	uint64_t localLogSeq{0};
	// Whether we should avoid emittings logs due to latency overhead (e.g., in IRQ/NMI context).
	std::atomic<bool> avoidEmittingLogs{false};
	// Bitmask of {RS_EMITTING, RS_PENDING} to determine whether we are currently emitting logs.
	std::atomic<unsigned int> reentrantLogState{0};

	unsigned int irqEntropySeq = 0;
	std::atomic<ProfileMechanism> profileMechanism{};
	// TODO: This should be a unique_ptr instead.
	SingleContextRecordRing *localProfileRing = nullptr;
};

inline CpuData *getCpuData() {
	return static_cast<CpuData *>(getPlatformCpuData());
}


extern "C" char percpuStart[], percpuEnd[];

template<typename T>
concept HasCpuDataConstructor = requires(CpuData *cpuData) {
	{ T{cpuData} };
};

// To add a new per-CPU variable, add a forward declaration like
// "extern PerCpu<Foo> foo;" in a header or source file, and then use
// THOR_DEFINE_PERCPU{,_UNINITIALIZED}(foo) in a source file to define it.
template <typename T>
struct PerCpu {
	T &get(CpuData *context) {
		auto offset =
			reinterpret_cast<uintptr_t>(&reservation)
			- reinterpret_cast<uintptr_t>(percpuStart);

		return *std::launder(reinterpret_cast<T *>(
					reinterpret_cast<uintptr_t>(context) + offset));
	}

	T &get() {
		return get(getCpuData());
	}

	T &getFor(size_t cpu) {
		auto size = percpuEnd - percpuStart;

		return *std::launder(reinterpret_cast<T *>(
					reinterpret_cast<uintptr_t>(&reservation) + size * cpu));
	}

	void initialize(CpuData *context) {
		auto offset =
			reinterpret_cast<uintptr_t>(&reservation)
			- reinterpret_cast<uintptr_t>(percpuStart);

		auto ptr = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(context) + offset);

		if constexpr (HasCpuDataConstructor<T>) {
			new(ptr) T{context};
		} else {
			new(ptr) T{}; // Value-initialize primitive types.
		}
	}

private:
	frg::aligned_storage<sizeof(T), alignof(T)> reservation;
};


using PerCpuInitializer = void(*)(CpuData *context);

template<auto *C>
void doInitializePerCpu(CpuData *context) {
	C->initialize(context);
}

#define THOR_DEFINE_PERCPU_INITIALIZER_PRIV(Name)			\
	[[gnu::section(".percpu_init"), gnu::used]]			\
	const constinit PerCpuInitializer Name ## _initializer_ = doInitializePerCpu<&Name>;

#define THOR_DEFINE_PERCPU_UNINITIALIZED_PRIV(Name, Suffix)	\
	[[gnu::section(".percpu" Suffix), gnu::used]]		\
	constinit decltype(Name) Name				\


// Define a per-CPU variable without an initializer. Care has to be
// taken to call Name.initialize(context) prior to accessing it from
// the given context. This is mainly intended for
// architecture-specific fields that have to be initialized prior to
// the allocator being available.
#define THOR_DEFINE_PERCPU_UNINITIALIZED(Name)		\
	THOR_DEFINE_PERCPU_UNINITIALIZED_PRIV(Name, "")	\

// Define a per-CPU variable that's initialized automatically. The
// initialization for the boot CPU happens after the kernel heap is
// available.
#define THOR_DEFINE_PERCPU(Name)			\
	THOR_DEFINE_PERCPU_UNINITIALIZED(Name);		\
	THOR_DEFINE_PERCPU_INITIALIZER_PRIV(Name)	\



extern PerCpu<CpuData> cpuData;

// Run initializers for the per-CPU variables of all CPUs.
void runCpuDataInitializers();


inline CpuData *getCpuData(size_t cpu) {
	return &cpuData.getFor(cpu);
}

size_t getCpuCount();

inline IrqMutex &irqMutex() {
	return getCpuData()->irqMutex;
}

inline ExecutorContext *currentExecutorContext() {
	return getCpuData()->executorContext;
}

} // namespace thor
