#pragma once

#include <netserver/nic.hpp>
#include <protocols/hw/client.hpp>

namespace nic::rtl8168 {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device);

}
