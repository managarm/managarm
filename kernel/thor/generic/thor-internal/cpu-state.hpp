#pragma once

#include <async/recurring-event.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

// Raised after a CPU transitions to CpuState::online.
async::recurring_event &cpuStateEvent();

// Try to skip an IPI to the given CPU with appropriate barriers.
[[nodiscard]] inline bool suppressIpiToOfflineCpu(CpuData *cpuData) {
	// A stale non-online state would wrongly skip a CPU, hence re-check behind a fence.
	// This pairs with the fence in setCpuState().
	if (cpuData->cpuState.load(std::memory_order_acquire) == CpuState::online) [[likely]]
		return false;
	std::atomic_thread_fence(std::memory_order_seq_cst);
	return cpuData->cpuState.load(std::memory_order_seq_cst) != CpuState::online;
}

// Advance the state of the given CPU.
void setCpuState(CpuData *cpuData, CpuState state);

} // namespace thor
