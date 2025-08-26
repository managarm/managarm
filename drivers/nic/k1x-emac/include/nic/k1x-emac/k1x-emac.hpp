#pragma once

#include <netserver/nic.hpp>

namespace nic::k1x_emac {

async::result<std::shared_ptr<nic::Link>> makeShared(mbus_ng::EntityId entity);

} // namespace nic::k1x_emac
