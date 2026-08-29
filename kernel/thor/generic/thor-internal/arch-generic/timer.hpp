#pragma once

#include <frg/optional.hpp>
#include <stdint.h>
#include <thor-internal/util.hpp>

namespace thor {

enum class UserspaceClockType {
	none,
	x86Tsc,
	armCntpct,
	armCntvct,
	riscvTime
};

struct UserspaceClock {
	UserspaceClockType type{UserspaceClockType::none};
	// Clock ticks to nanoseconds conversion factor.
	// Only meaningful if type is not UserspaceClockType::none.
	Pow2Fraction<Rounding::down> tickDuration{};
};

// Returns the number of nanoseconds elapsed on the monotonic clock.
uint64_t getClockNanos();
// Schedules an interrupt to fire once the monotonic clock reaches the
// deadline, or disarms the interrupt if deadline is frg::null_opt.
void setTimerDeadline(frg::optional<uint64_t> deadline);

// Returns whether the timer disarms itself once it reaches the deadline.
// This is true for edge-triggered timers. Level triggered timers keep asserting the interrupt until they are disarmed.
bool timerDisarmsItself();

// Returns whether timers are available and ready to use.
bool haveTimer();
// Get the raw timestamp in preemption timer ticks.
uint64_t getRawTimestampCounter();

// Returns the clock parameters for userspace or UserspaceClockType::none
// if getClockNanos() cannot be reproduced in userspace.
UserspaceClock getUserspaceClock();

// Called by the architecture-specific code. Handles timer deadline
// expiry.
void handleTimerInterrupt();

} // namespace thor
