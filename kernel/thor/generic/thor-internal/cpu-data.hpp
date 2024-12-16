#pragma once

#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/executor-context.hpp>
#include <thor-internal/kernel-locks.hpp>
#include <thor-internal/schedule.hpp>

#include <new>
#include <tuple>

namespace thor {

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

struct CpuData : public PlatformCpuData {
	static constexpr unsigned int RS_EMITTING = 1;
	static constexpr unsigned int RS_PENDING = 2;

	CpuData();

	CpuData(const CpuData &) = delete;

	CpuData &operator= (const CpuData &) = delete;

	IrqMutex irqMutex;
	UniqueKernelStack detachedStack;
	UniqueKernelStack idleStack;
	Scheduler scheduler;
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

		new(ptr) T{}; // Value-initialize primitive types.
	}

private:
	frg::aligned_storage<sizeof(T), alignof(T)> reservation;
};


using PerCpuInitializer = void(*)(CpuData *context);

#define THOR_DEFINE_PERCPU_INITIALIZER_PRIV(Name)			\
	[[gnu::section(".percpu_init"), gnu::used]]			\
	const constinit PerCpuInitializer Name ## _initializer_ =	\
		[] (CpuData *context) { Name.initialize(context); }	\

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

// Extend the per-CPU data area to make space for a new CPU, and run
// initializers for it.
// Returns a tuple of the pointer to the start of the new data, and
// it's index (e.g. for PerCpu<T>::getFor).
std::tuple<CpuData *, size_t> extendPerCpuData();

// Run initializers for the per-CPU variables of the boot CPU.
void runBootCpuDataInitializers();


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
