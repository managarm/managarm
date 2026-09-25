#pragma once

#include <stddef.h>
#include <stdint.h>

#include <frg/span.hpp>
#include <thor-internal/arch/idle.hpp>

namespace thor {

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
// Returns with interrupts disabled after an interrupt was taken.
void idleUntilInterrupt(const IdleMethod &method);

} // namespace thor
