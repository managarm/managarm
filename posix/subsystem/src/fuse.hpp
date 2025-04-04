#pragma once

#include "device.hpp"

namespace fuse {

std::shared_ptr<UnixDevice> createFuseDevice();

std::optional<std::shared_ptr<FsLink>> getFsRoot(std::shared_ptr<Process> proc, std::string arguments);

} //namespace fuse
