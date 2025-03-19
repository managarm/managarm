#pragma once

#include <async/result.hpp>
#include <helix/ipc.hpp>

namespace clk {

helix::BorrowedDescriptor trackerPageMemory();

async::result<void> enumerateTracker();

int64_t getRealtimeNanos();

struct timespec getRealtime();
struct timespec getTimeSinceBoot();

} // namespace clk
