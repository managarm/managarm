#pragma once

#include "api.hpp"
#include <helix/ipc.hpp>

namespace protocols::usb {

Device connect(helix::UniqueLane lane);

} // namespace protocols::usb
