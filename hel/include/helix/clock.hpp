#pragma once

#include <stdint.h>

namespace helix {

// Reads the system-wide monotone clock, i.e., the number of nanoseconds since boot.
// Equivalent to helGetClock() but avoids the syscall if the kernel exports a clock that userspace can read on its own.
uint64_t getClock();

// Whether getClock() reads the clock in userspace instead of falling back to helGetClock().
bool hasUserspaceClock();

} // namespace helix
