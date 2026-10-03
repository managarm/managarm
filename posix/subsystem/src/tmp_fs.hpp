#pragma once

#include "vfs.hpp"

namespace tmp_fs {

smarter::shared_ptr<FsNode> createMemoryNode(std::string path);

std::expected<smarter::shared_ptr<FsLink, LinkRc>, Error> createRoot(Process *p, std::string options);
smarter::shared_ptr<FsLink, LinkRc> createDevTmpFsRoot();

// Synchronously creates a device node and any missing parent directories.
// path is relative to root, which must be a directory of a tmpfs (e.g., the devtmpfs root).
// Fails if path or one of its parents exists but has the wrong type.
std::expected<void, Error>
createDevtmpfsNode(FsLink *root, std::string_view path, VfsType type, DeviceId id);

} // namespace tmp_fs
