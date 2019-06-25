#ifndef POSIX_SUBSYSTEM_DRM_SYSTEM_HPP
#define POSIX_SUBSYSTEM_DRM_SYSTEM_HPP

#include <cofiber.hpp>

namespace drm_subsystem {

async::detached run();

} // namespace drm_subsystem

#endif // POSIX_SUBSYSTEM_DRM_SYSTEM_HPP
