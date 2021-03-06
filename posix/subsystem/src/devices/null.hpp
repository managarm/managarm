#ifndef POSIX_SUBSYSTEM_DEVICES_NULL_HPP
#define POSIX_SUBSYSTEM_DEVICES_NULL_HPP

#include "../device.hpp"

std::shared_ptr<UnixDevice> createNullDevice();

#endif // POSIX_SUBSYSTEM_DEVICES_NULL_HPP
