#pragma once

#include <frg/optional.hpp>
#include <thor-internal/kernel-stack.hpp>

namespace thor {

struct IseqContext;
struct UserAccessRegion;

// Note: This struct is accessed from assembly.
// Do not change the field offsets!
struct AssemblyCpuData {
	AssemblyCpuData *selfPointer; //  0x0
	uint64_t currentDomain;       //  0x8
	void *exceptionStackPtr;      // 0x10
	void *irqStackPtr;            // 0x18
	uint64_t scratchSp;           // 0x20
	UserAccessRegion *currentUar; // 0x28
	IseqContext *iseqPtr;
};

inline void writeToTp(AssemblyCpuData *context) { asm volatile("mv tp, %0" : : "r"(context)); }

struct Executor;
struct Thread;

struct PlatformCpuData : public AssemblyCpuData {
	// Bits of the pendingIpis field.
	// Since RISC-V only has a single IPI vector, we need to emulate multiple IPIs in software.
	static constexpr uint64_t ipiPing = UINT64_C(1) << 0;
	static constexpr uint64_t ipiShootdown = UINT64_C(1) << 1;
	static constexpr uint64_t ipiSelfCall = UINT64_C(1) << 2;

	uint64_t hartId{~UINT64_C(0)};

	// Executor image that we use to save/restore state.
	Executor *activeExecutor{nullptr};

	// Actual value of the FS field in sstatus before it was cleared in the kernel.
	// Zero (= extOff) indicates that the current register state cannot be relied upon
	// (i.e., it has been saved to the executor or it is in control of U-mode).
	uint8_t stashedFs{0};

	std::atomic<uint64_t> pendingIpis;

	// Deadlines for global timers and preemption.
	// frg::null_opt is the respective deadline is not set (= infinite).
	frg::optional<uint64_t> timerDeadline;
	frg::optional<uint64_t> preemptionDeadline;
	// Current deadline programmed into the supervisor timer.
	uint64_t currentDeadline{~UINT64_C(0)};

	UniqueKernelStack irqStack;

	uint32_t profileFlags = 0;
};

// Get a pointer to this CPU's PlatformCpuData instance.
inline PlatformCpuData *getPlatformCpuData() {
	AssemblyCpuData *result;
	asm volatile("mv %0, tp" : "=r"(result));
	return static_cast<PlatformCpuData *>(result);
}

} // namespace thor
