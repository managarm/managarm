#ifndef POSIX_SUBSYSTEM_NET_HPP
#define POSIX_SUBSYSTEM_NET_HPP

#include <async/result.hpp>
#include <helix/ipc.hpp>

namespace net {

async::result<void> enumerateNetserver();
async::result<helix::BorrowedLane> getNetLane();

} // namespace net

#endif // POSIX_SUBSYSTEM_NET_HPP
