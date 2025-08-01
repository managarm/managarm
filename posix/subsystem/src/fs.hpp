#pragma once

#include <iostream>
#include <set>
#include <deque>
#include <unordered_map>

#include <async/result.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <core/id-allocator.hpp>
#include <frg/container_of.hpp>
#include <hel.h>
#include <sys/types.h>

#include <fcntl.h>

#include "file.hpp"

using DeviceId = std::pair<int, int>;

enum class VfsType {
	// null means that the file type is undefined in stat().
	// Avoid using null in favor of a more appropriate type.
	null, directory, regular, symlink, charDevice, blockDevice, socket, fifo
};

struct FileStats {
	uint64_t inodeNumber;
	int numLinks;
	uint64_t fileSize;
	uint32_t mode;
	int uid, gid;
	uint64_t atimeSecs, atimeNanos;
	uint64_t mtimeSecs, mtimeNanos;
	uint64_t ctimeSecs, ctimeNanos;
};

// Internal representation of struct statfs
struct FsFileStats {
	unsigned long f_type;
	// struct statfs has more members but we don't care about them yet
};


// Forward declarations.
struct FsLink;
struct FsNode;
struct ViewPath;

// ----------------------------------------------------------------------------
// FsLink class.
// ----------------------------------------------------------------------------

// Represents a directory entry on an actual file system (i.e. not in the VFS).
struct FsLink {
protected:
	~FsLink() = default;

public:
	virtual std::shared_ptr<FsNode> getOwner() = 0;
	virtual std::string getName() = 0;
	virtual std::shared_ptr<FsNode> getTarget() = 0;
	virtual async::result<frg::expected<Error>> obstruct();
	virtual std::optional<std::string> getProcFsDescription();
};

struct FsSuperblock {
protected:
	~FsSuperblock() = default;

public:
	virtual FutureMaybe<std::shared_ptr<FsNode>> createRegular(Process *) = 0;
	virtual FutureMaybe<std::shared_ptr<FsNode>> createSocket() = 0;

	virtual async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
			rename(FsLink *source, FsNode *directory, std::string name) = 0;
	virtual async::result<frg::expected<Error, FsFileStats>> getFsstats() = 0;
	virtual std::string getFsType() = 0;
	virtual dev_t deviceNumber() = 0;
};

FsSuperblock *getAnonymousSuperblock();

// This is used to allocate device IDs for non-device-based file systems such as tmpfs.
id_allocator<unsigned int> &getUnnamedDeviceIdAllocator();

// ----------------------------------------------------------------------------
// FsObserver class.
// ----------------------------------------------------------------------------

struct FsObserver {
protected:
	~FsObserver() = default;

public:
	static constexpr uint32_t deleteEvent = 1;
	static constexpr uint32_t deleteSelfEvent = 2;
	static constexpr uint32_t createEvent = 4;
	static constexpr uint32_t modifyEvent = 8;
	static constexpr uint32_t accessEvent = 16;
	static constexpr uint32_t closeWriteEvent = 32;
	static constexpr uint32_t closeNoWriteEvent = 64;
	static constexpr uint32_t ignoredEvent = 128;

	virtual void observeNotification(uint32_t events,
			const std::string &name, uint32_t cookie, bool isDir) = 0;
};

// ----------------------------------------------------------------------------
// FsNode class.
// ----------------------------------------------------------------------------

using SemanticFlags = uint32_t;
inline constexpr SemanticFlags semanticNonBlock = 1;
inline constexpr SemanticFlags semanticRead = 2;
inline constexpr SemanticFlags semanticWrite = 4;
inline constexpr SemanticFlags semanticAppend = 8;

// Represents an inode on an actual file system (i.e. not in the VFS).
struct FsNode {
	using DefaultOps = uint32_t;
	static inline constexpr DefaultOps defaultSupportsObservers = 1 << 1;

	FsNode(FsSuperblock *superblock, DefaultOps default_ops = 0)
	: _superblock{superblock}, _defaultOps{default_ops} {
		assert(_superblock);
	}

	FsSuperblock *superblock() {
		return _superblock;
	}

protected:
	~FsNode() = default;

public:
	virtual VfsType getType() = 0;

	// TODO: This should be async.
	virtual async::result<frg::expected<Error, FileStats>> getStats();

	// For directories only: Returns a pointer to the link
	// that links this directory from its parent.
	virtual std::shared_ptr<FsLink> treeLink();

	virtual void addObserver(std::shared_ptr<FsObserver> observer);

	virtual void removeObserver(FsObserver *observer);

	//! Get an existing link or create one (directories only).
	virtual async::result<std::expected<std::shared_ptr<FsLink>, Error>>
	getLinkOrCreate(Process *, std::string name, mode_t mode, bool exclusive = false);

	//! Resolves a file in a directory (directories only).
	virtual async::result<frg::expected<Error, std::shared_ptr<FsLink>>> getLink(std::string name);

	//! Links an existing node to this directory (directories only).
	virtual async::result<frg::expected<Error, std::shared_ptr<FsLink>>> link(std::string name,
			std::shared_ptr<FsNode> target);

	//! Creates a new directory (directories only).
	virtual async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	mkdir(std::string name);

	//! Creates a new symlink (directories only).
	virtual async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	symlink(std::string name, std::string path);

	//! Creates a new device file (directories only).
	virtual async::result<frg::expected<Error, std::shared_ptr<FsLink>>> mkdev(std::string name,
			VfsType type, DeviceId id);

	virtual async::result<frg::expected<Error, std::shared_ptr<FsLink>>> mkfifo(std::string name, mode_t mode);

	virtual async::result<frg::expected<Error>> unlink(std::string name);

	virtual async::result<frg::expected<Error>> rmdir(std::string name);

	//! Opens the file (regular files only).
	// TODO: Move this to the link instead of the inode?
	virtual async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags);

	// Reads the target of a symlink (symlinks only).
	// Returns illegalOperationTarget() by default.
	virtual expected<std::string> readSymlink(FsLink *link, Process *process);

	//! Read the major/minor device number (devices only).
	virtual DeviceId readDevice();

	// Changes permissions on a node
	virtual async::result<Error> chmod(int mode);

	// Changes timestamps on a node
	virtual async::result<Error> utimensat(std::optional<timespec> atime, std::optional<timespec> mtime, timespec ctime);

	// Creates an socket
	virtual async::result<frg::expected<Error, std::shared_ptr<FsLink>>> mksocket(std::string name);

	// Recursive path traversal
	virtual bool hasTraverseLinks();
	virtual async::result<frg::expected<Error, std::pair<std::shared_ptr<FsLink>, size_t>>> traverseLinks(std::deque<std::string> path);

	void notifyObservers(uint32_t inotifyEvents, const std::string &name, uint32_t cookie, bool isDir = false);

private:
	FsSuperblock *_superblock;
	DefaultOps _defaultOps;

	// Observers, for example for inotify.
	std::unordered_map<FsObserver *, std::shared_ptr<FsObserver>> _observers;
};

// ----------------------------------------------------------------------------
// SpecialLink class.
// ----------------------------------------------------------------------------

// This class can be used to construct FsLinks for anonymous special files
// such as epoll, signalfd, timerfd, etc.
struct SpecialLink final : FsLink, std::enable_shared_from_this<SpecialLink> {
private:
	struct PrivateTag { }; // To tag-dispatch to private methods.

public:
	static std::shared_ptr<SpecialLink> makeSpecialLink(VfsType fileType, int mode) {
		return std::make_shared<SpecialLink>(PrivateTag{}, fileType, mode);
	}

	SpecialLink(PrivateTag, VfsType fileType, int mode)
	: fileType_{fileType}, mode_{mode} { }

public:
	std::shared_ptr<FsNode> getTarget() override {
		return {shared_from_this(), &embeddedNode_};
	}

	std::shared_ptr<FsNode> getOwner() override {
		return nullptr;
	}

	std::string getName() override {
		throw std::runtime_error("SpecialLink has no name");
	}

	std::optional<std::string> getProcFsDescription() override {
		return "anon_inode:unimplemented";
	}

private:
	// SpecialLinks can never be linked into "real" file systems,
	// hence the can only ever be one link per node.
	struct EmbeddedNode final : FsNode {
		EmbeddedNode() : FsNode(getAnonymousSuperblock()) {}

		VfsType getType() override {
			auto node = frg::container_of(this, &SpecialLink::embeddedNode_);
			return node->fileType_;
		}

		async::result<frg::expected<Error, FileStats>> getStats() override {
			auto node = frg::container_of(this, &SpecialLink::embeddedNode_);
			FileStats stats{};
			// TODO: Allocate an inode number.
			stats.inodeNumber = 1;
			stats.fileSize = 0;
			stats.numLinks = 1;
			stats.mode = node->mode_;
			stats.uid = 0;
			stats.gid = 0;
			// TODO: Linux returns the current time for all timestamps.
			co_return stats;
		}
	};

	VfsType fileType_;
	int mode_;
	[[no_unique_address]] EmbeddedNode embeddedNode_;
};
