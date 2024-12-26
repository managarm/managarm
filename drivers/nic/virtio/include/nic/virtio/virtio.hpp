#pragma once

#include <netserver/nic.hpp>
#include <core/virtio/core.hpp>

namespace nic::virtio {
async::result<std::shared_ptr<nic::Link>> makeShared(mbus_ng::EntityId entity, std::unique_ptr<virtio_core::Transport>);
} // namespace nic::virtio
