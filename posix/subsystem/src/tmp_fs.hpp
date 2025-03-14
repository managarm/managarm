#pragma once

#include "vfs.hpp"

namespace tmp_fs {

std::shared_ptr<FsNode> createMemoryNode(std::string path);

std::shared_ptr<FsLink> createRoot();
std::shared_ptr<FsLink> createDevTmpFsRoot();

} // namespace tmp_fs
