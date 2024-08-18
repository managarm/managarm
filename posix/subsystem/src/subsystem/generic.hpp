#ifndef POSIX_SUBSYSTEM_GENERIC_SYSTEM_HPP
#define POSIX_SUBSYSTEM_GENERIC_SYSTEM_HPP

#include <cstdint>
#include <optional>
#include <protocols/mbus/client.hpp>
#include <string>

namespace generic_subsystem {

void run();

std::optional<std::pair<std::string, uint64_t>> getDeviceName(mbus_ng::EntityId id);

} // namespace generic_subsystem

#endif // POSIX_SUBSYSTEM_GENERIC_SYSTEM_HPP
