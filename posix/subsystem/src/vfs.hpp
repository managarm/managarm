
#ifndef POSIX_SUBSYSTEM_VFS_HPP
#define POSIX_SUBSYSTEM_VFS_HPP

#include <iostream>
#include <set>

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
using FutureMaybe = cofiber::future<T>;

// Forward declarations.
struct File;
struct FileOperations;

struct Link;
struct LinkOperations;

struct Node;
struct NodeOperations;

namespace _vfs_view {
	struct SharedView;
};

using _vfs_view::SharedView;

// ----------------------------------------------------------------------------
// File class.
// ----------------------------------------------------------------------------

struct File {
	File(const FileOperations *operations)
	: _operations(operations) { }

	const FileOperations *operations() {
		return _operations;
	}

private:
	const FileOperations *_operations;
};

struct FileOperations {
	FutureMaybe<off_t> (*seek)(std::shared_ptr<File> object, off_t offset, VfsSeek whence);
	FutureMaybe<size_t> (*readSome)(std::shared_ptr<File> object, void *data, size_t max_length);
	FutureMaybe<helix::UniqueDescriptor> (*accessMemory)(std::shared_ptr<File> object);
	helix::BorrowedDescriptor (*getPassthroughLane)(std::shared_ptr<File> object);
};

FutureMaybe<void> readExactly(std::shared_ptr<File> file, void *data, size_t length);

FutureMaybe<off_t> seek(std::shared_ptr<File> file, off_t offset, VfsSeek whence);

FutureMaybe<size_t> readSome(std::shared_ptr<File> file, void *data, size_t max_length);

FutureMaybe<helix::UniqueDescriptor> accessMemory(std::shared_ptr<File> file);

helix::BorrowedDescriptor getPassthroughLane(std::shared_ptr<File> file);

// ----------------------------------------------------------------------------
// Link class.
// ----------------------------------------------------------------------------

struct Link {
	Link(const LinkOperations *operations)
	: _operations(operations) { }

	const LinkOperations *operations() {
		return _operations;
	}

private:
	const LinkOperations *_operations;
};

struct LinkOperations {
	std::shared_ptr<Node> (*getOwner)(std::shared_ptr<Link> link);
	std::string (*getName)(std::shared_ptr<Link> link);
	std::shared_ptr<Node> (*getTarget)(std::shared_ptr<Link> link);
};

std::shared_ptr<Link> createRootLink(std::shared_ptr<Node> target);

std::shared_ptr<Node> getTarget(std::shared_ptr<Link> link);

// ----------------------------------------------------------------------------
// Node class.
// ----------------------------------------------------------------------------

struct Node {
	Node(const NodeOperations *operations)
	: _operations(operations) { }

	const NodeOperations *operations() {
		return _operations;
	}

private:
	const NodeOperations *_operations;
};

struct NodeOperations {
	VfsType (*getType)(std::shared_ptr<Node> node);

	//! Resolves a file in a directory (directories only).
	FutureMaybe<std::shared_ptr<Link>> (*getLink)(std::shared_ptr<Node> node, std::string name);

	//! Creates a new directory (directories only).
	FutureMaybe<std::shared_ptr<Link>> (*mkdir)(std::shared_ptr<Node> node, std::string name);
	
	//! Creates a new symlink (directories only).
	FutureMaybe<std::shared_ptr<Link>> (*symlink)(std::shared_ptr<Node> node,
			std::string name, std::string path);
	
	//! Creates a new device file (directories only).
	FutureMaybe<std::shared_ptr<Link>> (*mkdev)(std::shared_ptr<Node> node, std::string name,
			VfsType type, DeviceId id);
	
	//! Opens the file (regular files only).
	FutureMaybe<std::shared_ptr<File>> (*open)(std::shared_ptr<Node> node);
	
	//! Reads the target of a symlink (symlinks only).
	FutureMaybe<std::string> (*readSymlink)(std::shared_ptr<Node> node);

	//! Read the major/minor device number (devices only).
	DeviceId (*readDevice)(std::shared_ptr<Node> node);
};

VfsType getType(std::shared_ptr<Node> node);

FutureMaybe<std::shared_ptr<Link>> getLink(std::shared_ptr<Node> node, std::string name);

FutureMaybe<std::shared_ptr<Link>> mkdir(std::shared_ptr<Node> node, std::string name);

FutureMaybe<std::shared_ptr<Link>> symlink(std::shared_ptr<Node> node,
		std::string name, std::string path);

FutureMaybe<std::shared_ptr<Link>> mkdev(std::shared_ptr<Node> node, std::string name,
		VfsType type, DeviceId id);

FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<Node> node);

FutureMaybe<std::string> readSymlink(std::shared_ptr<Node> node);

DeviceId readDevice(std::shared_ptr<Node> node);

inline VfsType getDirectoryType(std::shared_ptr<Node>) {
	return VfsType::directory;
}
inline VfsType getRegularType(std::shared_ptr<Node>) {
	return VfsType::regular;
}
inline VfsType getSymlinkType(std::shared_ptr<Node>) {
	return VfsType::symlink;
}

namespace _vfs_view {

struct Data;

//! Represents a virtual view of the file system.
//! We handle all mount point related logic in this class.
struct SharedView {
	static SharedView createRoot(std::shared_ptr<Link> origin);

	SharedView() = default;

	explicit operator bool() {
		return (bool)_data;
	}

	std::shared_ptr<Link> getAnchor() const;
	std::shared_ptr<Link> getOrigin() const;

	void mount(std::shared_ptr<Link> anchor, std::shared_ptr<Link> origin) const;

	SharedView getMount(std::shared_ptr<Link> link) const;

private:
	explicit SharedView(std::shared_ptr<Data> data)
	: _data(std::move(data)) { }

	std::shared_ptr<Data> _data;
};

struct Compare {
	struct is_transparent { };

	bool operator() (const SharedView &a, const std::shared_ptr<Link> &b) const {
		return a.getAnchor() < b;
	}
	bool operator() (const std::shared_ptr<Link> &a, const SharedView &b) const {
		return a < b.getAnchor();
	}

	bool operator() (const SharedView &a, const SharedView &b) const {
		return a.getAnchor() < b.getAnchor();
	}
};

struct Data {
	SharedView parent;
	std::shared_ptr<Link> anchor;
	std::shared_ptr<Link> origin;
	std::set<SharedView, Compare> mounts;
};

} // namespace _vfs_view

FutureMaybe<std::shared_ptr<File>> open(std::string name);

#endif // POSIX_SUBSYSTEM_VFS_HPP

