#pragma once

#include <memory>

#include <protocols/fs/server.hpp>
#include <protocols/fs/file-locks.hpp>


namespace blockfs {

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

struct BaseFileSystem {
	// TODO(qookie): Ideally, these methods would be a part of the concept
	// instead of being pure virtual methods, but the code that uses these
	// methods currently is generic code that isn't templated on the FS type.
	virtual const protocols::fs::FileOperations *fileOps() = 0;
	virtual const protocols::fs::NodeOperations *nodeOps() = 0;

	virtual std::shared_ptr<void> accessRoot() = 0;
	virtual std::shared_ptr<void> accessInode(uint32_t inode) = 0;
	virtual async::result<std::shared_ptr<void>> createRegular(int uid, int gid, uint32_t parentIno) = 0;

	constexpr BaseFileSystem() = default;

	BaseFileSystem(const BaseFileSystem &) = delete;
	BaseFileSystem(BaseFileSystem &&) = delete;
	BaseFileSystem &operator=(const BaseFileSystem &) = delete;
	BaseFileSystem &operator=(BaseFileSystem &&) = delete;

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
