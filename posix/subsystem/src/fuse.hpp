#pragma once

#include "device.hpp"

namespace fuse {

std::shared_ptr<UnixDevice> createFuseDevice();

} //namespace fuse
