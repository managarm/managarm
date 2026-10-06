#pragma once

#include <stddef.h>
#include <stdint.h>

#include <frg/span.hpp>
#include <thor-internal/arch/idle.hpp>

namespace thor {

struct CpuData;

struct IdleState {
	// Name of the state in the vendor's terminology (e.g., C1E), for logging.
	const char *name;
	// Worst-case time (in ns) between the wake-up event and the first instruction that runs.
	uint64_t exitLatency;
	// Minimal time (in ns) that the CPU has to stay in this state to make entering it worthwhile.
	uint64_t targetResidency;
	// Whether the CPU loses the contents of its TLB in this state.
	bool losesTlb;
	// Only interpreted by idleUntilInterrupt().
	IdleMethod method;
};

inline constexpr size_t maxIdleStates = 8;

// Returns the idle states of the current CPU, ordered from shallowest to deepest.
// There is always at least one state and there are at most maxIdleStates.
frg::span<const IdleState> getIdleStates();

// Enters an idle state of the current CPU. Must be called with interrupts disabled.
// Returns with interrupts disabled after an interrupt was taken, after tryPingIdle() succeeded, or spuriously.
void idleUntilInterrupt(const IdleMethod &method);

// Wakes up cpu without an IPI if it idles in a state that supports this (e.g., mwait on x86).
// Returns false if cpu does not idle in such a state. The caller must send an IPI in this case.
// On success, the next consumeIdlePing() on the target CPU returns true.
// Stores that precede a successful tryPingIdle() on the caller CPU
// are ordered before loads that follow consumeIdlePing() on the target CPU.
bool tryPingIdle(CpuData *cpu);

// Returns (and clears) whether tryPingIdle() woke up the current CPU.
// Afterwards, tryPingIdle() fails until the next idleUntilInterrupt().
// The idle task must call this before it drains the scheduler's pending queue and before it is scheduled away
// (otherwise, tryPingIdle() keeps eliding IPIs while a thread runs on the CPU).
bool consumeIdlePing();

} // namespace thor
