#ifndef POSIX_SUBSYSTEM_DRM_SYSTEM_HPP
#define POSIX_SUBSYSTEM_DRM_SYSTEM_HPP

#include <cofiber.hpp>

namespace drm_system {

cofiber::no_future run();

} // namespace drm_system

#endif // POSIX_SUBSYSTEM_DRM_SYSTEM_HPP
