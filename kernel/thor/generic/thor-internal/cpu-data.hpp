#pragma once

#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/executor-context.hpp>
#include <thor-internal/kernel-locks.hpp>
#include <thor-internal/schedule.hpp>

#include <new>

namespace thor {

// Forward defined for pointers that are part of CpuData.
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

	ExecutorContext *executorContext = nullptr;
	KernelFiber *activeFiber;
	KernelFiber *wqFiber = nullptr;
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


struct PerCpuInitializer {
	void *target0;
	void (*init)(void *target);
};


extern "C" char percpuStart[], percpuEnd[];

template <typename T>
struct PerCpu {
	T &get() {
		auto offset =
			reinterpret_cast<uintptr_t>(&reservation)
			- reinterpret_cast<uintptr_t>(percpuStart);

		return *std::launder(reinterpret_cast<T *>(getLocalPerCpuBase() + offset));
	}

	T &getFor(int cpu) {
		auto size = percpuEnd - percpuStart;

		return *std::launder(reinterpret_cast<T *>(
					reinterpret_cast<uintptr_t>(&reservation) + size * cpu));
	}

	T &getInContext(void *context) {
		auto offset =
			reinterpret_cast<uintptr_t>(&reservation)
			- reinterpret_cast<uintptr_t>(percpuStart);

		return *std::launder(reinterpret_cast<T *>(
					reinterpret_cast<uintptr_t>(context) + offset));
	}

	void initializeInContext(void *context) {
		auto offset =
			reinterpret_cast<uintptr_t>(&reservation)
			- reinterpret_cast<uintptr_t>(percpuStart);

		auto ptr = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(context) + offset);

		new(ptr) T;
	}

private:
	frg::aligned_storage<sizeof(T), alignof(T)> reservation;
};

#define DEFINE_PERCPU_INITIALIZER_PRIV(Name, Type)			\
	[[gnu::section(".percpu_init"), gnu::used]]			\
	const inline PerCpuInitializer Name ## _initializer_ = {	\
		&Name,							\
		[] (void *target) { new(target) Type; }			\
	}								\

#define DEFINE_PERCPU_UNINIT_PRIV(Name, Type, Suffix)			\
	[[gnu::section(".percpu" Suffix), gnu::used]]			\
	inline PerCpu<Type> Name					\


// Define a per-CPU variable without an initializer. Care has to be
// taken to call Name.initializeInContext(context) prior to accessing
// it from the given context. This is mainly intended for
// architecture-specific fields that have to be initialized prior to
// the allocator being available.
#define DEFINE_PERCPU_UNINIT(Name, Type)		\
	DEFINE_PERCPU_UNINIT_PRIV(Name, Type, "")	\

// Define a per-CPU variable that's initialized automatically. The
// initialization for the boot CPU happens after the kernel heap is
// available.
#define DEFINE_PERCPU(Name, Type)			\
	DEFINE_PERCPU_UNINIT(Name, Type);		\
	DEFINE_PERCPU_INITIALIZER_PRIV(Name, Type)	\

DEFINE_PERCPU_UNINIT_PRIV(cpuData, CpuData, "_head");

// Extend the per-CPU data area to make space for a new CPU, and run
// initializers for it.
// Returns a pointer to the start of the new data.
void *addNewPerCpuData();

// Run initializers for the per-CPU variables of the boot CPU.
void runBootCpuDataInitializers();

inline CpuData *getCpuData() {
	return &cpuData.get();
}

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
