#pragma once

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include "api.hpp"

namespace protocols {
namespace usb {

async::detached serve(Device device, helix::UniqueLane lane);

} } // namespace protocols::usb
