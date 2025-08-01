#pragma once

#include <protocols/hw/client.hpp>
#include <netserver/nic.hpp>

namespace nic::pcnet {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device);

} // namespace nic::pcnet
