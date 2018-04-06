#ifndef POSIX_SUBSYSTEM_PTS_HPP
#define POSIX_SUBSYSTEM_PTS_HPP

#include "device.hpp"

namespace pts {

std::shared_ptr<UnixDevice> createMasterDevice();

std::shared_ptr<FsLink> getFsRoot();

} // namespace pts

#endif // POSIX_SUBSYSTEM_PTS_HPP
