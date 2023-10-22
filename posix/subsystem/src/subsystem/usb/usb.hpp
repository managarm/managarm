#pragma once

#include <async/result.hpp>
#include "../../drvcore.hpp"

namespace usb_subsystem {

async::detached run();

std::shared_ptr<drvcore::Device> getDeviceByMbus(int id);

} // namespace usb_subsystem
