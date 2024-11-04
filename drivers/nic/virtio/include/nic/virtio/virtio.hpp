#pragma once

#include <core/virtio/core.hpp>
#include <netserver/nic.hpp>

namespace nic::virtio {
std::shared_ptr<nic::Link> makeShared(std::unique_ptr<virtio_core::Transport>);
} // namespace nic::virtio
