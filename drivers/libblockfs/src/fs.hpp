#pragma once

#include <memory>
#include <unordered_set>

#include <async/mutex.hpp>

#include <protocols/fs/server.hpp>
#include <protocols/fs/file-locks.hpp>


namespace blockfs {

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

struct BaseFile {
	BaseFile(std::shared_ptr<void> inode, bool append)
	: inode{inode}, append{append} { }

	BaseFile(const BaseFile &) = delete;
	BaseFile(BaseFile &&) = delete;
	BaseFile &operator=(const BaseFile &) = delete;
	BaseFile &operator=(BaseFile &&) = delete;

	const std::shared_ptr<void> inode;
	async::shared_mutex mutex;

	uint64_t offset = 0;
	Flock flock;
	bool append;
};

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
concept File =
	std::derived_from<T, BaseFile>;

template <typename T>
concept FileSystem =
	std::derived_from<T, BaseFileSystem>
	&& requires {
		typename T::File;
		typename T::Inode;
}
	&& File<typename T::File>;


} // namespace blockfs
