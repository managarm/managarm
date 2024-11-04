#pragma once

#include "api.hpp"
#include <async/result.hpp>
#include <helix/ipc.hpp>

namespace protocols::usb {

async::detached serve(Device device, helix::UniqueLane lane);

} // namespace protocols::usb
