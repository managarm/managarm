#ifndef POSIX_SUBSYSTEM_FS_HPP
#define POSIX_SUBSYSTEM_FS_HPP

#include <iostream>
#include <set>
#include <deque>
#include <unordered_map>

#include <async/result.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <hel.h>

#include <fcntl.h>

#include "file.hpp"

using DeviceId = std::pair<int, int>;

enum class VfsType {
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


// Forward declarations.
struct FsLink;
struct FsNode;

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
};

struct FsSuperblock {
protected:
	~FsSuperblock() = default;

public:
	virtual FutureMaybe<std::shared_ptr<FsNode>> createRegular() = 0;
	virtual FutureMaybe<std::shared_ptr<FsNode>> createSocket() = 0;

	virtual async::result<std::shared_ptr<FsLink>> rename(FsLink *source,
			FsNode *directory, std::string name) = 0;
};

// ----------------------------------------------------------------------------
// FsObserver class.
// ----------------------------------------------------------------------------

struct FsObserver {
protected:
	~FsObserver() = default;

public:
	static constexpr uint32_t deleteEvent = 1;

	virtual void observeNotification(uint32_t events,
			const std::string &name, uint32_t cookie) = 0;
};

// ----------------------------------------------------------------------------
// FsNode class.
// ----------------------------------------------------------------------------

using SemanticFlags = uint32_t;
inline constexpr SemanticFlags semanticNonBlock = 1;
inline constexpr SemanticFlags semanticRead = 2;
inline constexpr SemanticFlags semanticWrite = 4;

// Represents an inode on an actual file system (i.e. not in the VFS).
struct FsNode {
	using DefaultOps = uint32_t;
	static inline constexpr DefaultOps defaultSupportsObservers = 1 << 1;

	// TODO: Remove this constructor once every FS has a superblock.
	FsNode(DefaultOps default_ops = 0)
	: _superblock{nullptr}, _defaultOps{default_ops} { }

	FsNode(FsSuperblock *superblock, DefaultOps default_ops = 0)
	: _superblock{superblock}, _defaultOps{default_ops} { }

	FsSuperblock *superblock() {
		return _superblock;
	}

protected:
	~FsNode() = default;

public:
	virtual VfsType getType();

	// TODO: This should be async.
	virtual FutureMaybe<FileStats> getStats();

	// For directories only: Returns a pointer to the link
	// that links this directory from its parent.
	virtual std::shared_ptr<FsLink> treeLink();

	virtual void addObserver(std::shared_ptr<FsObserver> observer);

	virtual void removeObserver(FsObserver *observer);

	//! Resolves a file in a directory (directories only).
	virtual FutureMaybe<std::shared_ptr<FsLink>> getLink(std::string name);
	
	//! Links an existing node to this directory (directories only).
	virtual FutureMaybe<std::shared_ptr<FsLink>> link(std::string name,
			std::shared_ptr<FsNode> target);

	//! Creates a new directory (directories only).
	virtual async::result<std::variant<Error, std::shared_ptr<FsLink>>> mkdir(std::string name);
	
	//! Creates a new symlink (directories only).
	virtual FutureMaybe<std::shared_ptr<FsLink>> symlink(std::string name, std::string path);
	
	//! Creates a new device file (directories only).
	virtual FutureMaybe<std::shared_ptr<FsLink>> mkdev(std::string name,
			VfsType type, DeviceId id);

	virtual FutureMaybe<std::shared_ptr<FsLink>> mkfifo(std::string name, mode_t mode);
	
	virtual FutureMaybe<void> unlink(std::string name);

	//! Opens the file (regular files only).
	// TODO: Move this to the link instead of the inode?
	virtual FutureMaybe<smarter::shared_ptr<File, FileHandle>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags);
	
	// Reads the target of a symlink (symlinks only).
	// Returns illegalOperationTarget() by default.
	virtual expected<std::string> readSymlink(FsLink *link);

	//! Read the major/minor device number (devices only).
	virtual DeviceId readDevice();

	// Changes permissions on a node
	virtual async::result<int> chmod(int mode);

protected:
	void notifyObservers(uint32_t inotifyEvents, const std::string &name, uint32_t cookie);

private:
	FsSuperblock *_superblock;
	DefaultOps _defaultOps;

	// Observers, for example for inotify.
	std::unordered_map<FsObserver *, std::shared_ptr<FsObserver>> _observers;
};

#endif // POSIX_SUBSYSTEM_FS_HPP
