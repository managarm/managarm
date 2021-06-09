#pragma once

#include "vfs.hpp"

namespace extern_fs {

std::shared_ptr<FsLink> createRoot(helix::UniqueLane sb_lane, helix::UniqueLane lane);

smarter::shared_ptr<File, FileHandle>
createFile(helix::UniqueLane lane, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link);

} // namespace extern_fs
