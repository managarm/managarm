#pragma once

#include <helix/ipc.hpp>
#include "api.hpp"

namespace protocols::usb {

Device connect(helix::UniqueLane lane);

} // namespace protocols::usb
