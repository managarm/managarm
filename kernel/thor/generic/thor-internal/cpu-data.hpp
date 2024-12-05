#pragma once

#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/executor-context.hpp>
#include <thor-internal/kernel-locks.hpp>
#include <thor-internal/schedule.hpp>

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

CpuData *getCpuData(size_t k);
size_t getCpuCount();

inline CpuData *getCpuData() {
	return static_cast<CpuData *>(getPlatformCpuData());
}

inline IrqMutex &irqMutex() {
	return getCpuData()->irqMutex;
}

inline ExecutorContext *currentExecutorContext() {
	return getCpuData()->executorContext;
}

} // namespace thor
