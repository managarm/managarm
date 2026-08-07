#pragma once

#include <frg/optional.hpp>
#include <stdint.h>

namespace thor {

// Returns the number of nanoseconds elapsed on the monotonic clock.
uint64_t getClockNanos();
// Schedules an interrupt to fire once the monotonic clock reaches the
// deadline, or disarms the interrupt if deadline is frg::null_opt.
void setTimerDeadline(frg::optional<uint64_t> deadline);
// Returns whether timers are available and ready to use.
bool haveTimer();
// Get the raw timestamp in preemption timer ticks.
uint64_t getRawTimestampCounter();

// Calibrate BogoMIPS against the architecture's raw timestamp counter.
// The result is scaled by 100 to preserve two decimal places.
uint64_t calibrateBogoMips(uint64_t frequencyHz);

// Called by the architecture-specific code. Handles timer deadline
// expiry.
void handleTimerInterrupt();

} // namespace thor
