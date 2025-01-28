#pragma once

#include <frg/optional.hpp>

namespace thor {

// Returns the number of nanoseconds elapsed on the monotonic clock.
uint64_t getClockNanos();

void setTimerDeadline(frg::optional<uint64_t> deadline);

} // namespace thor
