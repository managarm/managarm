
#ifndef POSIX_SUBSYSTEM_VFS_HPP
#define POSIX_SUBSYSTEM_VFS_HPP

#include <iostream>
#include <set>

#include <boost/intrusive/rbtree.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>
#include <hel.h>

//#include "device.hpp"

struct Process;

enum VfsError {
	success, eof
};

enum class VfsType {
	null, directory, symlink, regular
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
namespace _vfs_file {
	struct FileData;
	struct SharedFile;
}

using _vfs_file::FileData;
using _vfs_file::SharedFile;

namespace _vfs_node {
	struct LinkData;
	struct SharedLink;

	struct NodeData;
	struct TreeData;
	struct BlobData;
	struct RegularData;
	struct SymlinkData;
	struct SharedNode;
}

using _vfs_node::LinkData;
using _vfs_node::SharedLink;

using _vfs_node::NodeData;
using _vfs_node::TreeData;
using _vfs_node::BlobData;
using _vfs_node::RegularData;
using _vfs_node::SymlinkData;
using _vfs_node::SharedNode;

namespace _vfs_view {
	struct SharedView;
};

using _vfs_view::SharedView;

// ----------------------------------------------------------------------------
// SharedFile class.
// ----------------------------------------------------------------------------

namespace _vfs_file {

struct FileData {
	virtual FutureMaybe<off_t> seek(off_t offset, VfsSeek whence) = 0;

	virtual FutureMaybe<size_t> readSome(void *data, size_t max_length) = 0;
	
	virtual FutureMaybe<helix::UniqueDescriptor> accessMemory() = 0;

	virtual helix::BorrowedDescriptor getPassthroughLane() = 0;
};

//! Represents an open file.
struct SharedFile {
	FutureMaybe<void> readExactly(void *data, size_t length) const;
	
	FutureMaybe<off_t> seek(off_t offset, VfsSeek whence) const;

	FutureMaybe<size_t> readSome(void *data, size_t max_length) const;
	
	FutureMaybe<helix::UniqueDescriptor> accessMemory() const;

	helix::BorrowedDescriptor getPassthroughLane() const;

	explicit SharedFile(std::shared_ptr<FileData> data)
	: _data(std::move(data)) { }

private:
	std::shared_ptr<FileData> _data;
};

} // namespace _vfs_file

namespace _vfs_node {

struct LinkData {
	virtual SharedNode getOwner() = 0;
	virtual std::string getName() = 0;
	virtual SharedNode getTarget() = 0;
};

//! Represents a directory entry on a physical/pseudo file system.
struct SharedLink {
	static SharedLink createRoot(SharedNode target);

	SharedLink() = default;

	explicit SharedLink(std::shared_ptr<LinkData> data)
	: _data(std::move(data)) { }

	bool operator< (const SharedLink &other) const;

	SharedNode getTarget() const;

private:
	std::shared_ptr<LinkData> _data;
};

struct NodeData {
	virtual VfsType getType() = 0;
};

struct TreeData : NodeData {
	VfsType getType() override {
		return VfsType::directory;
	}

	virtual FutureMaybe<SharedLink> getLink(std::string name) = 0;
	
	virtual FutureMaybe<SharedLink> mkdir(std::string name) = 0;

	virtual FutureMaybe<SharedLink> symlink(std::string name, std::string path) = 0;
};

struct BlobData : NodeData { };

struct RegularData : BlobData {
	VfsType getType() override {
		return VfsType::regular;
	}

	virtual FutureMaybe<SharedFile> open() = 0;
};

struct SymlinkData : BlobData {
	VfsType getType() override {
		return VfsType::symlink;
	}

	virtual FutureMaybe<std::string> readSymlink() = 0;
};

//! Represents a file on a physical/pseudo file system.
struct SharedNode {
	SharedNode() = default;

	explicit SharedNode(std::shared_ptr<NodeData> data)
	: _data(std::move(data)) { };

	bool operator< (const SharedNode &other) const;

	VfsType getType() const;

	//! Resolves a file in a directory (directories only).
	FutureMaybe<SharedLink> getLink(std::string name) const;

	//! Creates a new directory (directories only).
	FutureMaybe<SharedLink> mkdir(std::string name) const;
	
	//! Creates a new symlink (directories only).
	FutureMaybe<SharedLink> symlink(std::string name, std::string path) const;
	
	//! Opens the file (regular files only).
	FutureMaybe<SharedFile> open() const;
	
	//! Reads the target of a symlink (symlinks only).
	FutureMaybe<std::string> readSymlink() const;

private:
	std::shared_ptr<NodeData> _data;
};

} // namespace _vfs_node

namespace _vfs_view {

struct Data;

//! Represents a virtual view of the file system.
//! We handle all mount point related logic in this class.
struct SharedView {
	static SharedView createRoot(SharedLink origin);

	SharedView() = default;

	explicit operator bool() {
		return (bool)_data;
	}

	SharedLink getAnchor() const;
	SharedLink getOrigin() const;

	void mount(SharedLink anchor, SharedLink origin) const;

	SharedView getMount(SharedLink link) const;

private:
	explicit SharedView(std::shared_ptr<Data> data)
	: _data(std::move(data)) { }

	std::shared_ptr<Data> _data;
};

struct Compare {
	struct is_transparent { };

	bool operator() (const SharedView &a, const SharedLink &b) const {
		return a.getAnchor() < b;
	}
	bool operator() (const SharedLink &a, const SharedView &b) const {
		return a < b.getAnchor();
	}

	bool operator() (const SharedView &a, const SharedView &b) const {
		return a.getAnchor() < b.getAnchor();
	}
};

struct Data {
	SharedView parent;
	SharedLink anchor;
	SharedLink origin;
	std::set<SharedView, Compare> mounts;
};

} // namespace _vfs_view

FutureMaybe<SharedFile> open(std::string name);

#endif // POSIX_SUBSYSTEM_VFS_HPP

