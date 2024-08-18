#pragma once

#include <async/result.hpp>
#include <protocols/mbus/client.hpp>

#include "../../drvcore.hpp"

namespace usb_subsystem {

async::detached run();

async::result<std::shared_ptr<drvcore::Device>> getInterfaceDevice(std::shared_ptr<drvcore::Device>, mbus_ng::Properties &);

} // namespace usb_subsystem
