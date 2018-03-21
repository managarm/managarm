#ifndef POSIX_SUBSYSTEM_DRVCORE_HPP
#define POSIX_SUBSYSTEM_DRVCORE_HPP

#include <string>

namespace drvcore {

void initialize();

void emitHotplug(std::string buffer);

} // namespace drvcore

#endif // POSIX_SUBSYSTEM_DRVCORE_HPP
