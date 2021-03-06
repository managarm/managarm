#ifndef POSIX_SUBSYSTEM_PROCFS_HPP
#define POSIX_SUBSYSTEM_PROCFS_HPP

#include <protocols/fs/server.hpp>

#include "vfs.hpp"

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
	virtual async::result<std::string> show() = 0;
	virtual async::result<void> store(std::string buffer) = 0;
};

struct DirectoryNode final : FsNode, std::enable_shared_from_this<DirectoryNode> {
	friend struct DirectoryFile;

	static std::shared_ptr<Link> createRootDirectory();

	DirectoryNode();

	std::shared_ptr<Link> directMkregular(std::string name,
			std::shared_ptr<RegularNode> regular);
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

} // namespace procfs

std::shared_ptr<FsLink> getProcfs();

#endif // POSIX_SUBSYSTEM_PROCFS_HPP
