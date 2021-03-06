#ifndef POSIX_SUBSYSTEM_DEVICES_ZERO_HPP
#define POSIX_SUBSYSTEM_DEVICES_ZERO_HPP

#include "../device.hpp"

std::shared_ptr<UnixDevice> createZeroDevice();

#endif // POSIX_SUBSYSTEM_DEVICES_ZERO_HPP
