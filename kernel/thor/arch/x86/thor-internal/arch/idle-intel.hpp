#pragma once

#include <stdint.h>

#include <frg/span.hpp>

namespace thor {

struct MwaitState {
	const char *name;
	uint32_t hint;
	uint64_t exitLatency;
	uint64_t targetResidency;
	bool losesTlb;
};

// Returns the MWAIT states of the current CPU if we know its model (and an empty span otherwise).
// Also applies the model's MSR quirks (unless we run under a hypervisor).
// Requires an Intel CPU with MWAIT support.
frg::span<const MwaitState> initializeIntelMwaitStates();

} // namespace thor
