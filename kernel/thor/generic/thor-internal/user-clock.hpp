#pragma once

#include <smarter.hpp>

namespace thor {

struct MemoryView;

// Memory object that exports the parameters of the monotonic clock to userspace, such that
// userspace can compute getClockNanos() without performing a syscall.
smarter::shared_ptr<MemoryView> getUserClockMemory();

} // namespace thor
