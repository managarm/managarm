#pragma once

#include <protocols/fs/server.hpp>

#include "vfs.hpp"

struct Process;
struct FileDescriptor;

namespace cgroupfs {

struct LinkCompare;
struct Link;
struct DirectoryNode;

// ----------------------------------------------------------------------------
// FS data structures.
// This API is only intended for private use.
// ----------------------------------------------------------------------------

struct LinkCompare {
	struct is_transparent { };

	bool operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const;
	bool operator() (const std::shared_ptr<Link> &link, const std::string &name) const;
	bool operator() (const std::string &name, const std::shared_ptr<Link> &link) const;
};

struct RegularFile final : File {
public:
	static void serve(smarter::shared_ptr<RegularFile> file);

	explicit RegularFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link);

	void handleClose() override;

	async::result<frg::expected<Error, off_t>> seek(off_t offset, VfsSeek whence) override;

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t max_length) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

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

struct RegularNode : FsNode, std::enable_shared_from_this<RegularNode> {
	friend struct RegularFile;

	RegularNode();
	virtual ~RegularNode() = default;

	async::result<VfsType> getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override;

protected:
	virtual async::result<std::string> show() = 0;
	virtual async::result<void> store(std::string buffer) = 0;
};

struct SuperBlock final : FsSuperblock {
public:
	SuperBlock() {
		deviceMinor_ = getUnnamedDeviceIdAllocator().allocate();
	}

	FutureMaybe<std::shared_ptr<FsNode>> createRegular(Process *) override;
	FutureMaybe<std::shared_ptr<FsNode>> createSocket() override;

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
			rename(FsLink *source, FsNode *directory, std::string name) override;
	async::result<frg::expected<Error, FsFileStats>> getFsstats() override;

	std::string getFsType() override {
		return "cgroup2";
	}

	dev_t deviceNumber() override {
		return makedev(0, deviceMinor_);
	}

private:
	unsigned int deviceMinor_;
};

struct DirectoryNode final : FsNode, std::enable_shared_from_this<DirectoryNode> {
	friend struct DirectoryFile;

	static std::shared_ptr<Link> createRootDirectory();

	DirectoryNode();

	std::shared_ptr<Link> directMkregular(std::string name,
			std::shared_ptr<RegularNode> regular);
	std::shared_ptr<Link> directMknode(std::string name,
			std::shared_ptr<FsNode> node);
	std::shared_ptr<Link> directMkdir(std::string name);
	async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	mkdir(std::string name) override;

	async::result<VfsType> getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	std::shared_ptr<FsLink> treeLink() override;

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> link(std::string name,
			std::shared_ptr<FsNode> target) override;

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override;
	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> getLink(std::string name) override;
	async::result<frg::expected<Error>> unlink(std::string name) override;

	std::shared_ptr<Link> createCgroupDirectory(std::string name);
	void createCgroupFiles();

private:
	Link *_treeLink;
	std::set<std::shared_ptr<Link>, LinkCompare> _entries;
};

struct ProcsNode final : RegularNode {
	ProcsNode() {}

	async::result<std::string> show() override;
	async::result<void> store(std::string) override;
private:
	// Process *_process;
};

struct ControllersNode final : RegularNode {
	ControllersNode() {}

	async::result<std::string> show() override;
	async::result<void> store(std::string) override;
private:
	// Process *_process;
};

struct LinkNode : FsNode, std::enable_shared_from_this<LinkNode> {
	LinkNode();

	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "\e[31mposix: Fix cgroupfs LinkNode::getStats()\e[39m" << std::endl;
		co_return FileStats{};
	}


	async::result<VfsType> getType() override {
		co_return VfsType::symlink;
	}
};

} // namespace cgroupfs

std::shared_ptr<FsLink> getCgroupfs();
