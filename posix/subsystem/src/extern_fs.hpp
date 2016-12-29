
#ifndef POSIX_SUBSYSTEM_EXTERN_FS_HPP
#define POSIX_SUBSYSTEM_EXTERN_FS_HPP

#include "vfs.hpp"

namespace extern_fs {

std::shared_ptr<Link> createRoot();
std::shared_ptr<Link> createRoot(helix::UniqueLane lane);

} // namespace extern_fs

#endif // POSIX_SUBSYSTEM_EXTERN_FS_HPP

