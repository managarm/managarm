#ifndef POSIX_SUBSYSTEM_CLOCK_HPP
#define POSIX_SUBSYSTEM_CLOCK_HPP

#include <async/result.hpp>
#include <helix/ipc.hpp>

namespace clk {

helix::BorrowedDescriptor trackerPageMemory();

async::result<void> enumerateTracker();

struct timespec getRealtime();

} // namespace clk

#endif // POSIX_SUBSYSTEM_CLOCK_HPP
