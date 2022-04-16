#pragma once

#include <netserver/nic.hpp>
#include <protocols/hw/client.hpp>
#include <helix/memory.hpp>

namespace nic::e1000 {
std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device, helix::Mapping regSpace, helix::UniqueDescriptor bar);
} // namespace nic::e1000
