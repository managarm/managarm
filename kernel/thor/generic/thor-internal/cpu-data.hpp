#pragma once

#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/executor-context.hpp>
#include <thor-internal/ring-buffer.hpp>
#include <thor-internal/schedule.hpp>

namespace thor {

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
