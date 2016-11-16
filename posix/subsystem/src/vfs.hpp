
#ifndef POSIX_SUBSYSTEM_VFS_HPP
#define POSIX_SUBSYSTEM_VFS_HPP

#include <iostream>

#include <boost/intrusive/rbtree.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>
#include <hel.h>

//#include "device.hpp"

struct Process;

enum VfsError {
	success, eof
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
	struct SharedFile;
}

using _vfs_file::SharedFile;

namespace _vfs_node {
	struct SharedEntry; struct SharedNode;
}

using _vfs_node::SharedEntry;
using _vfs_node::SharedNode;

// ----------------------------------------------------------------------------
// SharedFile class.
// Stores a pointer to the associated SharedEntry which in turn stores a pointer
// to the SharedNode that the SharedFile operates on.
// ----------------------------------------------------------------------------

namespace _vfs_file {

struct Data;

struct SharedFile {
	template<typename T, typename... Args>
	static SharedFile create(SharedEntry entry, Args &&... args);
	
	FutureMaybe<void> readExactly(void *data, size_t length) const;
	
	FutureMaybe<off_t> seek(off_t offset, VfsSeek whence) const;

	FutureMaybe<size_t> readSome(void *data, size_t max_length) const;
	
	FutureMaybe<helix::UniqueDescriptor> accessMemory() const;

	helix::BorrowedDescriptor getPassthroughLane() const;

private:
	explicit SharedFile(std::shared_ptr<Data> data)
	: _data(std::move(data)) { }

	std::shared_ptr<Data> _data;
};

} // namespace _vfs_file

// ----------------------------------------------------------------------------
// SharedNode + SharedEntry classes.
// SharedNodes come in two flavors: directories
// and regular nodes (e.g. regular files, symlinks and device files).
// These classes mirror the physical inodes.
// ----------------------------------------------------------------------------

namespace _vfs_node {

enum class Type {
	null, directory, regular
};

struct EntryData;

struct NodeData;
struct DirectoryNodeData;
struct RegularNodeData;

struct SharedEntry {
friend class SharedNode;
	static SharedEntry attach(SharedNode parent, std::string name,
			SharedNode target);

	SharedEntry() = default;

	const std::string &getName() const;

	SharedNode getTarget() const;

private:
	explicit SharedEntry(std::shared_ptr<EntryData> data)
	: _data(std::move(data)) { }

	std::shared_ptr<EntryData> _data;
};

struct SharedNode {
friend class EntryData;
	template<typename T, typename... Args>
	static SharedNode createDirectory(Args &&... args);

	template<typename T, typename... Args>
	static SharedNode createRegular(Args &&... args);

	SharedNode() = default;

	FutureMaybe<SharedFile> open(SharedEntry entry);

	FutureMaybe<SharedEntry> getChild(std::string name);

private:
	explicit SharedNode(std::shared_ptr<NodeData> data)
	: _data(std::move(data)) { }

	std::shared_ptr<NodeData> _data;
};

} // namespace _vfs_node

// ----------------------------------------------------------------------------
// SharedFile details.
// ----------------------------------------------------------------------------

namespace _vfs_file {

struct Data {
	explicit Data(SharedEntry entry)
	: entry(std::move(entry)) { }
	
	virtual FutureMaybe<off_t> seek(off_t offset, VfsSeek whence) = 0;

	virtual FutureMaybe<size_t> readSome(void *data, size_t max_length) = 0;
	
	virtual FutureMaybe<helix::UniqueDescriptor> accessMemory() = 0;

	virtual helix::BorrowedDescriptor getPassthroughLane() = 0;

	const SharedEntry entry;
};

template<typename T, typename... Args>
SharedFile SharedFile::create(SharedEntry entry, Args &&... args) {
	struct Derived : Data {
		Derived(SharedEntry entry, Args &&... args)
		: Data(std::move(entry)), _delegate(std::forward<Args>(args)...) { }
	
		FutureMaybe<off_t> seek(off_t offset, VfsSeek whence) override {
			return _delegate.seek(offset, whence);
		}
	
		FutureMaybe<size_t> readSome(void *data, size_t max_length) override {
			return _delegate.readSome(data, max_length);
		}
		
		FutureMaybe<helix::UniqueDescriptor> accessMemory() override {
			return _delegate.accessMemory();
		}

		helix::BorrowedDescriptor getPassthroughLane() override {
			return _delegate.getPassthroughLane();
		}

	private:
		T _delegate;
	};

	auto data = std::make_shared<Derived>(std::move(entry), std::forward<Args>(args)...);
	return SharedFile(std::move(data));
}

} // namespace _vfs_file

// ----------------------------------------------------------------------------
// SharedNode + SharedEntry details.
// ----------------------------------------------------------------------------

namespace _vfs_node {

struct EntryData : std::enable_shared_from_this<EntryData> {
	explicit EntryData(SharedNode parent, std::string name, SharedNode target);

	~EntryData() {
		std::cout << "EntryData: Destructor called!" << std::endl;
		__builtin_trap();
	}

	bool operator< (const EntryData &other) const {
		return name < other.name;
	}

	const SharedNode parent;

	const std::string name;

	SharedNode target;

	boost::intrusive::set_member_hook<> dirHook;
};

struct NodeData {
	virtual Type getType() = 0;
};

struct DirectoryNodeData : NodeData {
	Type getType() override {
		return Type::directory;
	}

	virtual FutureMaybe<SharedNode> resolveChild(std::string name) = 0;

	boost::intrusive::rbtree<
		EntryData,
		boost::intrusive::member_hook<
			EntryData,
			boost::intrusive::set_member_hook<>,
			&EntryData::dirHook
		>
	> dirElements;
};

struct RegularNodeData : NodeData {
	Type getType() override {
		return Type::regular;
	}

	virtual FutureMaybe<SharedFile> open(SharedEntry entry) = 0;
};

template<typename T, typename... Args>
SharedNode SharedNode::createDirectory(Args &&... args) {
	struct Derived : DirectoryNodeData {
		Derived(Args &&... args)
		: _delegate(std::forward<Args>(args)...) { }

		FutureMaybe<SharedNode> resolveChild(std::string name) override {
			return _delegate.resolveChild(std::move(name));
		}

	private:
		T _delegate;
	};

	auto data = std::make_shared<Derived>(std::forward<Args>(args)...);
	return SharedNode(std::move(data));
}

template<typename T, typename... Args>
SharedNode SharedNode::createRegular(Args &&... args) {
	struct Derived : RegularNodeData {
		Derived(Args &&... args)
		: _delegate(std::forward<Args>(args)...) { }

		FutureMaybe<SharedFile> open(SharedEntry entry) override {
			return _delegate.open(std::move(entry));
		}

	private:
		T _delegate;
	};

	auto data = std::make_shared<Derived>(std::forward<Args>(args)...);
	return SharedNode(std::move(data));
}

} // namespace _vfs_node

FutureMaybe<SharedFile> open(std::string name);

#endif // POSIX_SUBSYSTEM_VFS_HPP

