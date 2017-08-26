
#ifndef POSIX_SUBSYSTEM_VFS_HPP
#define POSIX_SUBSYSTEM_VFS_HPP

#include <iostream>
#include <set>

#include <async/result.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>
#include <hel.h>

struct Process;

using DeviceId = std::pair<int, int>;

enum VfsError {
	success, eof
};

enum class VfsType {
	null, directory, regular, symlink, charDevice, blockDevice
};

enum class VfsSeek {
	null, absolute, relative, eof
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

template<typename T>
using FutureMaybe = async::result<T>;

// Forward declarations.
struct File;
struct FileOperations;

struct Link;
struct LinkOperations;

struct Node;
struct NodeOperations;

namespace _vfs_view {
	struct MountView;
};

using _vfs_view::MountView;

// ----------------------------------------------------------------------------
// File class.
// ----------------------------------------------------------------------------

struct File {
	File(std::shared_ptr<Node> node)
	: _node{std::move(node)} { }

	std::shared_ptr<Node> node() {
		return _node;
	}

	FutureMaybe<void> readExactly(void *data, size_t length);

	virtual FutureMaybe<off_t> seek(off_t offset, VfsSeek whence) = 0;
	virtual FutureMaybe<size_t> readSome(void *data, size_t max_length) = 0;
	virtual FutureMaybe<helix::UniqueDescriptor> accessMemory() = 0;
	virtual helix::BorrowedDescriptor getPassthroughLane() = 0;

private:
	const std::shared_ptr<Node> _node;
};

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
	virtual FutureMaybe<std::shared_ptr<File>> open();
	
	//! Reads the target of a symlink (symlinks only).
	virtual FutureMaybe<std::string> readSymlink();

	//! Read the major/minor device number (devices only).
	virtual DeviceId readDevice();

private:
};

namespace _vfs_view {

//! Represents a virtual view of the file system.
//! We handle all mount point related logic in this class.
struct MountView : std::enable_shared_from_this<MountView> {
	static std::shared_ptr<MountView> createRoot(std::shared_ptr<Link> origin);

	// TODO: This is an implementation detail that could be hidden.
	explicit MountView(std::shared_ptr<MountView> parent, std::shared_ptr<Link> anchor,
			std::shared_ptr<Link> origin)
	: _parent{std::move(parent)}, _anchor{std::move(anchor)}, _origin{std::move(origin)} { }

	std::shared_ptr<Link> getAnchor() const;
	std::shared_ptr<Link> getOrigin() const;

	void mount(std::shared_ptr<Link> anchor, std::shared_ptr<Link> origin);

	std::shared_ptr<MountView> getMount(std::shared_ptr<Link> link) const;

private:
	struct Compare {
		struct is_transparent { };

		bool operator() (const std::shared_ptr<MountView> &a, const std::shared_ptr<Link> &b) const {
			return a->getAnchor() < b;
		}
		bool operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<MountView> &b) const {
			return a < b->getAnchor();
		}

		bool operator() (const std::shared_ptr<MountView> &a, const std::shared_ptr<MountView> &b) const {
			return a->getAnchor() < b->getAnchor();
		}
	};

	std::shared_ptr<MountView> _parent;
	std::shared_ptr<Link> _anchor;
	std::shared_ptr<Link> _origin;
	std::set<std::shared_ptr<MountView>, Compare> _mounts;
};

} // namespace _vfs_view

using ViewPath = std::pair<std::shared_ptr<MountView>, std::shared_ptr<Link>>;

async::result<void> populateRootView();

ViewPath rootPath();

FutureMaybe<ViewPath> resolve(ViewPath root, std::string name);

FutureMaybe<std::shared_ptr<File>> open(ViewPath root, std::string name);

#endif // POSIX_SUBSYSTEM_VFS_HPP

