#ifndef POSIX_SUBSYSTEM_PTS_HPP
#define POSIX_SUBSYSTEM_PTS_HPP

#include "device.hpp"

namespace pts {

std::shared_ptr<UnixDevice> createMasterDevice();

} // namespace pts

#endif // POSIX_SUBSYSTEM_PTS_HPP
