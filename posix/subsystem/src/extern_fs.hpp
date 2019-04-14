
#ifndef POSIX_SUBSYSTEM_EXTERN_FS_HPP
#define POSIX_SUBSYSTEM_EXTERN_FS_HPP

#include "vfs.hpp"

namespace extern_fs {

std::shared_ptr<FsLink> createRoot(helix::UniqueLane sb_lane, helix::UniqueLane lane);

smarter::shared_ptr<File, FileHandle>
createFile(helix::UniqueLane lane, std::shared_ptr<FsLink> link);

} // namespace extern_fs

#endif // POSIX_SUBSYSTEM_EXTERN_FS_HPP

