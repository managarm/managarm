#pragma once

#include <async/result.hpp>
#include <netserver/nic.hpp>
#include <protocols/hw/client.hpp>

namespace nic::rtl8168 {

async::result<std::shared_ptr<nic::Link>> makeShared(protocols::hw::Device device);

}
