#pragma once

#include "vfs.hpp"

namespace tmp_fs {

std::shared_ptr<FsNode> createMemoryNode(std::string path);

std::shared_ptr<FsLink> createRoot();

} // namespace tmp_fs
