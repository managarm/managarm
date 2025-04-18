#pragma once

#include <stdint.h>
#include <time.h>

namespace posix {

uint64_t convertToNanos(const timespec &ts, clockid_t clock, bool relative = false);

} // namespace posix
