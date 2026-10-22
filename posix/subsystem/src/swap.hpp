#pragma once

#include <async/result.hpp>
#include <helix/ipc.hpp>

async::result<void> enumerateSwapserver();

helix::BorrowedLane getSwapserverLane();
helix::BorrowedDescriptor getSwapSpaceHandle();
