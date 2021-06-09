#pragma once

#include <helix/ipc.hpp>
#include "api.hpp"

namespace protocols {
namespace usb {

Device connect(helix::UniqueLane lane);

} } // namespace protocols::usb
