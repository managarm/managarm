#pragma once

#include <netserver/nic.hpp>
#include <protocols/hw/client.hpp>

namespace nic::igc {

async::result<std::shared_ptr<nic::Link>> makeShared(protocols::hw::Device device);

} // namespace nic::igc
