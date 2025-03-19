#pragma once

#include <protocols/fs/server.hpp>

#include "vfs.hpp"

struct Process;
struct FileDescriptor;

namespace procfs {

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

	async::result<frg::expected<Error, PollStatusResult>> pollStatus(Process *) override;

	async::result<frg::expected<Error, PollWaitResult>> pollWait(Process *,
		uint64_t sequence, int mask,
		async::cancellation_token cancellation = {}) override;

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

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override;

protected:
	virtual async::result<std::string> show(Process *) = 0;
	virtual async::result<void> store(std::string buffer) = 0;

	async::result<frg::expected<Error, FileStats>> getStatsInternal(Process *);
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
		return "proc";
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

	template<typename T>
	requires requires (T t) { {t._treeLink} -> std::same_as<Link *&>; }
	std::shared_ptr<Link> directMknodeDir(std::string name, std::shared_ptr<T> node);

	std::shared_ptr<Link> directMknode(std::string name,
			std::shared_ptr<FsNode> node);
	std::shared_ptr<Link> directMkdir(std::string name);
	std::shared_ptr<Link> createProcDirectory(std::string name, Process *process);

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	std::shared_ptr<FsLink> treeLink() override;

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> link(std::string name,
			std::shared_ptr<FsNode> target) override;

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override;
	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> getLink(std::string name) override;
	async::result<frg::expected<Error>> unlink(std::string name) override;

private:
	Link *_treeLink;
	std::set<std::shared_ptr<Link>, LinkCompare> _entries;
};

struct LinkNode : FsNode, std::enable_shared_from_this<LinkNode> {
	LinkNode();

	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "\e[31mposix: Fix procfs LinkNode::getStats()\e[39m" << std::endl;
		co_return FileStats{};
	}


	VfsType getType() override {
		return VfsType::symlink;
	}

protected:
	async::result<frg::expected<Error, FileStats>> getStatsInternal(Process *);
};

struct SelfLink final : LinkNode, std::enable_shared_from_this<SelfLink> {
	SelfLink() = default;

	expected<std::string> readSymlink(FsLink *link, Process *process) override;
};

struct SelfThreadLink final : LinkNode, std::enable_shared_from_this<SelfThreadLink> {
	SelfThreadLink() = default;

	expected<std::string> readSymlink(FsLink *link, Process *process) override;
};

struct ExeLink final : LinkNode, std::enable_shared_from_this<ExeLink> {
	ExeLink(Process *process)
	: _process(process)
	{ }

	expected<std::string> readSymlink(FsLink *link, Process *process) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	Process *_process;
};

struct RootLink final : LinkNode, std::enable_shared_from_this<RootLink> {
	RootLink(Process *process)
	: _process(process)
	{ }

	expected<std::string> readSymlink(FsLink *link, Process *process) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	Process *_process;
};

struct CwdLink final : LinkNode, std::enable_shared_from_this<CwdLink> {
	CwdLink(Process *process)
	: _process(process)
	{ }

	expected<std::string> readSymlink(FsLink *link, Process *process) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	Process *_process;
};

struct MountsLink final : LinkNode, std::enable_shared_from_this<MountsLink> {
	MountsLink() = default;

	expected<std::string> readSymlink(FsLink *link, Process *process) override;
};

struct MapNode final : RegularNode {
	MapNode(Process *process)
	: _process(process)
	{ }

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	Process *_process;
};

struct UptimeNode final : RegularNode {
	UptimeNode() {}

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;
};

struct OstypeNode final : RegularNode {
	OstypeNode() {}

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;
};

struct OsreleaseNode final : RegularNode {
	OsreleaseNode() {}

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;
};

struct ArchNode final : RegularNode {
	ArchNode() {}

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;
};

struct BootIdNode final : RegularNode {
	BootIdNode();

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;
private:
	std::string bootId_;
};

struct CommNode final : RegularNode {
	CommNode(Process *process)
	: _process(process)
	{ }

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	Process *_process;
};

struct StatNode final : RegularNode {
	StatNode(Process *process)
	: _process(process)
	{ }

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	Process *_process;
};

struct StatmNode final : RegularNode {
	StatmNode(Process *process)
	: _process(process)
	{ }

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	Process *_process;
};

struct StatusNode final : RegularNode {
	StatusNode(Process *process)
	: _process(process)
	{ }

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	Process *_process;
};

struct FdDirectoryFile final : File {
public:
	static void serve(smarter::shared_ptr<FdDirectoryFile> file);

	explicit FdDirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, Process *process);

	void handleClose() override;

	FutureMaybe<ReadEntriesResult> readEntries() override;
	helix::BorrowedDescriptor getPassthroughLane() override;

private:
	Process *_process;

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	std::unordered_map<int, FileDescriptor> _fileTable;
	std::unordered_map<int, FileDescriptor>::const_iterator _iter;
};

struct CgroupNode final : RegularNode {
	CgroupNode(Process *process)
	: _process(process)
	{ }

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;
private:
	Process *_process;
};

struct FdDirectoryNode final : FsNode, std::enable_shared_from_this<FdDirectoryNode> {
public:
	friend DirectoryNode;

	explicit FdDirectoryNode(Process *process);

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override;
	std::shared_ptr<FsLink> treeLink() override;
	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> getLink(std::string name) override;
private:
	Process *_process;
	Link *_treeLink;
};

struct SymlinkNode final : LinkNode, std::enable_shared_from_this<SymlinkNode> {
	SymlinkNode(std::shared_ptr<MountView>, std::weak_ptr<FsLink>);

	expected<std::string> readSymlink(FsLink *link, Process *process) override;

private:
	std::shared_ptr<MountView> _mount;
	std::weak_ptr<FsLink> _link;
};

struct MountsNode final : RegularNode {
	MountsNode(Process *process)
	: _process(process)
	{ }

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;

private:
	Process *_process;
};

struct MountInfoNode final : RegularNode {
	MountInfoNode(Process *process)
	: _process(process)
	{ }

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;

	async::result<frg::expected<Error, FileStats>> getStats() override;

private:
	Process *_process;
};

struct FdInfoDirectoryNode final : FsNode, std::enable_shared_from_this<FdInfoDirectoryNode> {
public:
	friend DirectoryNode;

	explicit FdInfoDirectoryNode(Process *process);

	VfsType getType() override;
	async::result<frg::expected<Error, FileStats>> getStats() override;
	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override;
	std::shared_ptr<FsLink> treeLink() override;
	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> getLink(std::string name) override;
private:
	Process *_process;
	Link *_treeLink;
};

struct FdInfoDirectoryFile final : File {
public:
	static void serve(smarter::shared_ptr<FdInfoDirectoryFile> file);

	explicit FdInfoDirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, Process *process);

	void handleClose() override;

	FutureMaybe<ReadEntriesResult> readEntries() override;
	helix::BorrowedDescriptor getPassthroughLane() override;

private:
	Process *_process;

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	std::unordered_map<int, FileDescriptor> _fileTable;
	std::unordered_map<int, FileDescriptor>::const_iterator _iter;
};

struct FdInfoNode final : RegularNode {
	FdInfoNode(std::shared_ptr<MountView> mountView, smarter::shared_ptr<File, FileHandle> file)
	: mountView_{std::move(mountView)}, file_{std::move(file)} {}

	async::result<std::string> show(Process *) override;
	async::result<void> store(std::string) override;
private:
	std::shared_ptr<MountView> mountView_;
	smarter::shared_ptr<File, FileHandle> file_;
};

} // namespace procfs

std::shared_ptr<FsLink> getProcfs();
