#pragma once

#include <frg/spinlock.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

void initializeIrqVectors();

inline bool intsAreEnabled() {
	uint64_t v;
	asm volatile ("mrs %0, daif" : "=r"(v));
	return !v;
}

inline void enableInts() {
	asm volatile ("msr daifclr, #15");
}

inline void disableInts() {
	asm volatile ("msr daifset, #15");
}

inline void halt() {
	asm volatile ("wfi");
}

void suspendSelf();

} // namespace thor
