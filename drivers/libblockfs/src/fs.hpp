#pragma once

#include "common.hpp"
#include <memory>
#include <unordered_set>

#include <async/mutex.hpp>

#include <protocols/fs/server.hpp>
#include <protocols/fs/file-locks.hpp>


namespace blockfs {

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

struct BaseFileSystem;

struct BaseInode {
	BaseInode(BaseFileSystem &fs, uint32_t number)
	: fs{fs}, number{number} {  }

	BaseFileSystem &fs;
	const uint32_t number;

	async::oneshot_event readyEvent;

	int uid, gid;
	FileType fileType;

	FlockManager flockManager;
	std::unordered_set<std::string> obstructedLinks;
};

struct BaseFile {
	BaseFile(std::shared_ptr<void> inode, bool append)
	: inode{inode}, append{append} { }

	const std::shared_ptr<void> inode;
	async::shared_mutex mutex;

	uint64_t offset = 0;
	Flock flock;
	bool append;
};

struct BaseFileSystem {
	virtual const protocols::fs::FileOperations *fileOps() = 0;
	virtual const protocols::fs::NodeOperations *nodeOps() = 0;

	virtual std::shared_ptr<void> accessRoot() = 0;
	virtual std::shared_ptr<void> accessInode(uint32_t inode) = 0;
	virtual async::result<std::shared_ptr<void>> createRegular(int uid, int gid) = 0;

	virtual ~BaseFileSystem() = default;
};

template <typename T>
concept Inode =
	std::derived_from<T, BaseInode>
	&& requires (T ino) {
		{ ino.fileSize() } -> std::same_as<size_t>;
		{ ino.accessMemory() } -> std::same_as<helix::BorrowedDescriptor>;
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
	&& File<typename T::File>
	&& Inode<typename T::Inode>;


} // namespace blockfs
