#pragma once

#include "vfs.hpp"

namespace tmp_fs {

std::shared_ptr<FsNode> createMemoryNode(std::string path);

std::expected<std::shared_ptr<FsLink>, Error> createRoot(Process *p, std::string options);
std::shared_ptr<FsLink> createDevTmpFsRoot();

} // namespace tmp_fs
