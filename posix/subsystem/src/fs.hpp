#ifndef POSIX_SUBSYSTEM_FS_HPP
#define POSIX_SUBSYSTEM_FS_HPP

#include <iostream>
#include <set>

#include <async/result.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>
#include <hel.h>

#include "file.hpp"
#include "fs.hpp"

using DeviceId = std::pair<int, int>;

enum class VfsType {
	null, directory, regular, symlink, charDevice, blockDevice
};

struct FileStats {
	uint64_t inodeNumber;
	uint32_t mode;
	int numLinks;
	int uid, gid;
	uint64_t fileSize;
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
	virtual std::shared_ptr<FsNode> getOwner() = 0;
	virtual std::string getName() = 0;
	virtual std::shared_ptr<FsNode> getTarget() = 0;
};

// ----------------------------------------------------------------------------
// FsNode class.
// ----------------------------------------------------------------------------

// Represents an inode on an actual file system (i.e. not in the VFS).
struct FsNode {
	virtual VfsType getType();

	// TODO: This should be async.
	virtual FutureMaybe<FileStats> getStats();

	//! Resolves a file in a directory (directories only).
	virtual FutureMaybe<std::shared_ptr<FsLink>> getLink(std::string name);
	
	//! Links an existing node to this directory (directories only).
	virtual FutureMaybe<std::shared_ptr<FsLink>> link(std::string name,
			std::shared_ptr<FsNode> target);

	//! Creates a new directory (directories only).
	virtual FutureMaybe<std::shared_ptr<FsLink>> mkdir(std::string name);
	
	//! Creates a new symlink (directories only).
	virtual FutureMaybe<std::shared_ptr<FsLink>> symlink(std::string name, std::string path);
	
	//! Creates a new device file (directories only).
	virtual FutureMaybe<std::shared_ptr<FsLink>> mkdev(std::string name,
			VfsType type, DeviceId id);

	//! Opens the file (regular files only).
	// TODO: Move this to the link instead of the inode?
	virtual FutureMaybe<std::shared_ptr<ProperFile>> open(std::shared_ptr<FsLink> link);
	
	//! Reads the target of a symlink (symlinks only).
	virtual FutureMaybe<std::string> readSymlink();

	//! Read the major/minor device number (devices only).
	virtual DeviceId readDevice();

private:
};

#endif // POSIX_SUBSYSTEM_FS_HPP
