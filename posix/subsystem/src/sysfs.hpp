#pragma once

#include <protocols/fs/server.hpp>

#include "vfs.hpp"

struct Process;

namespace sysfs {

struct LinkCompare;
struct Link;
struct DirectoryNode;

struct Attribute;
struct Object;
struct Hierarchy;

// ----------------------------------------------------------------------------
// FS data structures.
// This API is only intended for private use.
// ----------------------------------------------------------------------------

struct SysfsSuperblock final : FsSuperblock {
public:
	SysfsSuperblock() = default;

	FutureMaybe<std::shared_ptr<FsNode>> createRegular(Process *) override;
	FutureMaybe<std::shared_ptr<FsNode>> createSocket() override;

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
			rename(FsLink *source, FsNode *directory, std::string name) override;
};

struct LinkCompare {
	struct is_transparent { };

	bool operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const;
	bool operator() (const std::shared_ptr<Link> &link, const std::string &name) const;
	bool operator() (const std::string &name, const std::shared_ptr<Link> &link) const;
};

struct AttributeFile final : File {
public:
	static void serve(smarter::shared_ptr<AttributeFile> file);

	explicit AttributeFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link);

	void handleClose() override;

	async::result<frg::expected<Error, off_t>> seek(off_t offset, VfsSeek whence) override;

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t max_length) override;

	async::result<frg::expected<Error, size_t>>
	pread(Process *, int64_t offset, void *buffer, size_t length) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override;

	helix::BorrowedDescriptor getPassthroughLane() override;

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	bool _cached;
	std::string _buffer;
	size_t _offset;
};

struct DirectoryFile final : File {
public:
	static void serve(smarter::shared_ptr<DirectoryFile> file);

	explicit DirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link);

	void handleClose() override;

	FutureMaybe<ReadEntriesResult> readEntries() override;
	helix::BorrowedDescriptor getPassthroughLane() override;
	async::result<frg::expected<Error, off_t>> seek(off_t offset, VfsSeek whence) override;

private:
	// TODO: Remove this and extract it from the associatedLink().
	DirectoryNode *_node;

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	std::set<std::shared_ptr<Link>, LinkCompare>::iterator _iter;
};

struct Link final : FsLink, std::enable_shared_from_this<Link> {
	explicit Link(std::shared_ptr<FsNode> target);

	explicit Link(std::shared_ptr<FsNode> owner,
			std::string name, std::shared_ptr<FsNode> target);

	std::shared_ptr<FsNode> getOwner() override;
	std::string getName() override;
	std::shared_ptr<FsNode> getTarget() override;

private:
	std::shared_ptr<FsNode> _owner;
	std::string _name;
	std::shared_ptr<FsNode> _target;
};

struct AttributeNode final : FsNode, std::enable_shared_from_this<AttributeNode> {
	friend struct AttributeFile;

	AttributeNode(Object *object, Attribute *attr);

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override;

private:
	Object *_object;
	Attribute *_attr;
};

struct SymlinkNode final : FsNode, std::enable_shared_from_this<SymlinkNode> {
	SymlinkNode(std::weak_ptr<Object> target);

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	expected<std::string> readSymlink(FsLink *link, Process *process) override;

private:
	std::weak_ptr<Object> _target;
};

struct DirectoryNode final : FsNode, std::enable_shared_from_this<DirectoryNode> {
	friend struct DirectoryFile;

	static std::shared_ptr<Link> createRootDirectory();

	DirectoryNode();

	std::shared_ptr<Link> directMkattr(Object *object, Attribute *attr);
	std::shared_ptr<Link> directMklink(std::string name, std::weak_ptr<Object> target);
	std::shared_ptr<Link> directMkdir(std::string name);

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	std::shared_ptr<FsLink> treeLink() override;

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override;
	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> getLink(std::string name) override;

private:
	Link *_treeLink;
	std::set<std::shared_ptr<Link>, LinkCompare> _entries;
};

// ----------------------------------------------------------------------------
// Object abstraction.
// Subsystems should use this API to manage sysfs.
// ----------------------------------------------------------------------------

struct Attribute {
	Attribute(std::string name, bool writable);
	Attribute(std::string name, bool writable, size_t size);

	virtual ~Attribute() = default;

public:
	const std::string &name() {
		return _name;
	}

	bool writable() {
		return _writable;
	}

	size_t size() {
		return _size;
	}

	virtual async::result<frg::expected<Error, std::string>> show(Object *object) = 0;
	virtual async::result<Error> store(Object *object, std::string data);
	virtual async::result<frg::expected<Error, helix::UniqueDescriptor>> accessMemory(Object *object);

protected:
	size_t _size = 4096;
private:
	const std::string _name;
	bool _writable;
};

// Object corresponds to Linux kobjects.
struct Object {
	Object(std::shared_ptr<Object> parent, std::string name);
	virtual ~Object() = default;

	const std::string &name() {
		return _name;
	}

	std::shared_ptr<DirectoryNode> directoryNode();

	void realizeAttribute(Attribute *attr);
	void createSymlink(std::string name, std::shared_ptr<Object> target);

	virtual std::optional<std::string> getClassPath();
	void addObject();

private:
	std::shared_ptr<Object> _parent;
	std::string _name;

	std::shared_ptr<Link> _dirLink;
};

// Hierarchy corresponds to Linux ksets.
struct Hierarchy {

};

} // namespace sysfs

std::shared_ptr<FsLink> getSysfs();
