#pragma once

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include "api.hpp"

namespace protocols::usb {

async::detached serve(Device device, helix::UniqueLane lane);

} // namespace protocols::usb
