#pragma once

#include <async/result.hpp>
#include <helix/ipc.hpp>

namespace net {

async::result<void> enumerateNetserver();
async::result<helix::BorrowedLane> getNetLane();

} // namespace net
