#pragma once

#include <memory>

#include <protocols/fs/server.hpp>
#include <protocols/fs/file-locks.hpp>


namespace blockfs {

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

struct BaseFileSystem {
	virtual const protocols::fs::FileOperations *fileOps() = 0;
	virtual const protocols::fs::NodeOperations *nodeOps() = 0;

	virtual std::shared_ptr<void> accessRoot() = 0;
	virtual std::shared_ptr<void> accessInode(uint32_t inode) = 0;
	virtual async::result<std::shared_ptr<void>> createRegular(int uid, int gid) = 0;

	virtual ~BaseFileSystem() = default;
};

template <typename T>
concept FileSystem =
	std::derived_from<T, BaseFileSystem>
	&& requires {
		typename T::File;
		typename T::Inode;
};

} // namespace blockfs
