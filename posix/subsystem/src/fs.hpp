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
struct Link;
struct Node;

// ----------------------------------------------------------------------------
// Link class.
// ----------------------------------------------------------------------------

struct Link {
	virtual std::shared_ptr<Node> getOwner() = 0;
	virtual std::string getName() = 0;
	virtual std::shared_ptr<Node> getTarget() = 0;
};

std::shared_ptr<Link> createRootLink(std::shared_ptr<Node> target);

// ----------------------------------------------------------------------------
// Node class.
// ----------------------------------------------------------------------------

struct Node {
	virtual VfsType getType();

	// TODO: This should be async.
	virtual FileStats getStats();

	//! Resolves a file in a directory (directories only).
	virtual FutureMaybe<std::shared_ptr<Link>> getLink(std::string name);
	
	//! Links an existing node to this directory (directories only).
	virtual FutureMaybe<std::shared_ptr<Link>> link(std::string name,
			std::shared_ptr<Node> target);

	//! Creates a new directory (directories only).
	virtual FutureMaybe<std::shared_ptr<Link>> mkdir(std::string name);
	
	//! Creates a new symlink (directories only).
	virtual FutureMaybe<std::shared_ptr<Link>> symlink(std::string name, std::string path);
	
	//! Creates a new device file (directories only).
	virtual FutureMaybe<std::shared_ptr<Link>> mkdev(std::string name,
			VfsType type, DeviceId id);
	
	//! Opens the file (regular files only).
	virtual FutureMaybe<std::shared_ptr<ProperFile>> open(std::shared_ptr<Link> link);
	
	//! Reads the target of a symlink (symlinks only).
	virtual FutureMaybe<std::string> readSymlink();

	//! Read the major/minor device number (devices only).
	virtual DeviceId readDevice();

private:
};

#endif // POSIX_SUBSYSTEM_FS_HPP
