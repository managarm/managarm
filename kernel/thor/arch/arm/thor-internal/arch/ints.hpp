#pragma once

#include <frg/spinlock.hpp>
#include <thor-internal/debug.hpp>

#include <thor-internal/cpu-data.hpp>
#include <thor-internal/arch-generic/timer.hpp>

namespace thor {

inline bool intsAreEnabled() {
	uint64_t v;
	asm volatile("mrs %0, daif" : "=r"(v));
	return !v;
}

inline void enableInts() {
	auto now = getClockNanos();
	auto disabled = getCpuData()->disableIrqsStamp;
	auto elapsed = now - disabled;
	if (disabled && elapsed > 50'000'000)
		urgentLogger() << "thor: !!!!!!!!!!!!!!!!!! IRQs reenabled after 100ms (" << elapsed << " ns)" << frg::endlog;
	asm volatile("msr daifclr, #15");
	getCpuData()->disableIrqsStamp = 0;
}

inline void disableInts() {
	getCpuData()->disableIrqsStamp = getClockNanos();
	asm volatile("msr daifset, #15");
}

inline void halt() { asm volatile("wfi"); }

void suspendSelf();

} // namespace thor
