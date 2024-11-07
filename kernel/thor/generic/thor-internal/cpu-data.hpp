#pragma once

#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/executor-context.hpp>
#include <thor-internal/kernel-locks.hpp>
#include <thor-internal/schedule.hpp>

namespace thor {

// Forward defined for pointers that are part of CpuData.
struct KernelFiber;
struct SingleContextRecordRing;
struct WorkQueue;

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
	UniqueKernelStack detachedStack;
	UniqueKernelStack idleStack;
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
