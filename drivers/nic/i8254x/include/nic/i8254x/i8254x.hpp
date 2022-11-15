#pragma once

#include <netserver/nic.hpp>
#include <protocols/hw/client.hpp>

namespace nic::intel8254x {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device);

}
