#pragma once

#include <netserver/nic.hpp>

namespace nic::bcmgenet {

async::result<std::shared_ptr<nic::Link>> makeShared(mbus_ng::EntityId entity);

} // namespace nic::bcmgenet
