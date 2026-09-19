#pragma once

#include <async/recurring-event.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

// Raised after a CPU transitions to CpuState::online.
async::recurring_event &cpuStateEvent();

// Advance the state of the given CPU.
void setCpuState(CpuData *cpuData, CpuState state);

} // namespace thor
