#pragma once

#include "device.hpp"

namespace pts {

std::shared_ptr<UnixDevice> createMasterDevice();

std::shared_ptr<FsLink> getFsRoot();

} // namespace pts
