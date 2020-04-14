
#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "extern_fs.hpp"
#include "fs.pb.h"

namespace extern_fs {

namespace {

struct Node;
struct DirectoryNode;

struct Superblock final : FsSuperblock {
	Superblock(helix::UniqueLane lane);

	FutureMaybe<std::shared_ptr<FsNode>> createRegular() override;
	FutureMaybe<std::shared_ptr<FsNode>> createSocket() override;

	async::result<std::shared_ptr<FsLink>> rename(FsLink *source,
			FsNode *directory, std::string name) override;

	std::shared_ptr<Node> internalizeStructural(uint64_t id, helix::UniqueLane lane);
	std::shared_ptr<Node> internalizeStructural(Node *owner, std::string name,
			uint64_t id, helix::UniqueLane lane);
	std::shared_ptr<Node> internalizePeripheralNode(int64_t type, int id, helix::UniqueLane lane);
	std::shared_ptr<FsLink> internalizePeripheralLink(Node *parent, std::string name,
			std::shared_ptr<Node> target);

private:
	helix::UniqueLane _lane;
	std::map<uint64_t, std::weak_ptr<DirectoryNode>> _activeStructural;
	std::map<uint64_t, std::weak_ptr<Node>> _activePeripheralNodes;
	std::map<std::pair<Node *, std::string>, std::weak_ptr<FsLink>> _activePeripheralLinks;
};

struct Node : FsNode {
	async::result<FileStats> getStats() override {
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_GET_STATS);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(getLane(), helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp));
		co_await transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		FileStats stats{};
		stats.inodeNumber = getInode(); // TODO: Move this out of FileStats.
		stats.fileSize = resp.file_size();
		stats.numLinks = resp.num_links();
		stats.mode = 0; //resp.mode(); // TODO: Fix this after fixing modes in mlibc.
		stats.uid = resp.uid();
		stats.gid = resp.gid();
		stats.atimeSecs = resp.atime_secs();
		stats.atimeNanos = resp.atime_nanos();
		stats.ctimeSecs = resp.mtime_secs();
		stats.atimeNanos = resp.mtime_nanos();
		stats.ctimeSecs = resp.ctime_secs();
		stats.atimeNanos = resp.ctime_nanos();

		co_return stats;
	}

public:
	Node(uint64_t inode, helix::UniqueLane lane, Superblock *sb = nullptr)
	: FsNode{sb}, _inode{inode}, _lane{std::move(lane)} { }

protected:
	~Node() = default;

public:
	uint64_t getInode() {
		return _inode;
	}

	helix::BorrowedLane getLane() {
		return _lane;
	}

	void setupWeakNode(std::weak_ptr<Node> self) {
		_self = std::move(self);
	}

	std::weak_ptr<Node> weakNode() {
		return _self;
	}

private:
	std::weak_ptr<Node> _self;
	uint64_t _inode;
	helix::UniqueLane _lane;
};

struct OpenFile final : File {
private:
	expected<off_t> seek(off_t offset, VfsSeek whence) override {
		assert(whence == VfsSeek::absolute);
		co_await _file.seekAbsolute(offset);
		co_return offset;
	}

	// TODO: Ensure that the process is null? Pass credentials of the thread in the request?
	expected<size_t>
	readSome(Process *, void *data, size_t max_length) override {
		size_t length = co_await _file.readSome(data, max_length);
		co_return length;
	}

	// TODO: For extern_fs, we can simply return POLLIN | POLLOUT here.
	// Move device code out of this file.
	expected<PollResult> poll(Process *, uint64_t sequence,
			async::cancellation_token cancellation) override {
		auto result = co_await _file.poll(sequence, cancellation);
		co_return result;
	}

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override {
		auto memory = co_await _file.accessMemory();
		co_return std::move(memory);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _file.getLane();
	}

public:
	OpenFile(helix::UniqueLane control, helix::UniqueLane lane,
			std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: File{StructName::get("externfs.file"), std::move(mount), std::move(link)},
			_control{std::move(control)}, _file{std::move(lane)} { }

	~OpenFile() {
		// It's not necessary to do any cleanup here.
	}

	void handleClose() override {
		// Close the control lane to inform the server that we closed the file.
		_control = helix::UniqueLane{};
	}

private:
	helix::UniqueLane _control;
	protocols::fs::File _file;
};

struct RegularNode final : Node {
private:
	VfsType getType() override {
		return VfsType::regular;
	}

	FutureMaybe<SharedFilePtr>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		// Regular files do not support O_NONBLOCK.
		semantic_flags &= ~semanticNonBlock;

		assert(!semantic_flags);
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::PullDescriptor pull_ctrl;
		helix::PullDescriptor pull_passthrough;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_OPEN);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(getLane(), helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&pull_ctrl, kHelItemChain),
				helix::action(&pull_passthrough));
		co_await transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(pull_ctrl.error());
		HEL_CHECK(pull_passthrough.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		auto file = smarter::make_shared<OpenFile>(pull_ctrl.descriptor(),
				pull_passthrough.descriptor(), std::move(mount), std::move(link));
		file->setupWeakFile(file);
		co_return File::constructHandle(std::move(file));
	}

public:
	RegularNode(Superblock *sb, uint64_t inode, helix::UniqueLane lane)
	: Node{inode, std::move(lane), sb}, _sb{sb} { }

private:
	Superblock *_sb;
};

struct SymlinkNode final : Node {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	expected<std::string> readSymlink(FsLink *) override {
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::RecvInline recv_target;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_READ_SYMLINK);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(getLane(), helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&recv_target));
		co_await transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(recv_target.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		co_return std::string{static_cast<char *>(recv_target.data()), recv_target.length()};
	}

public:
	SymlinkNode(uint64_t inode, helix::UniqueLane lane)
	: Node{inode, std::move(lane)} { }
};

struct Link : FsLink {
private:
	std::shared_ptr<FsNode> getOwner() override {
		return _owner;
	}

	std::string getName() override {
		assert(_owner);
		return _name;
	}

public:
	Link() = default;

	Link(std::shared_ptr<FsNode> owner, std::string name)
	: _owner{std::move(owner)}, _name{std::move(name)} {
		assert(_owner);
	}

protected:
	~Link() = default;

private:
	std::shared_ptr<FsNode> _owner;
	std::string _name;
};

// This class maintains a strong reference to the target.
struct PeripheralLink final : Link {
private:
	std::shared_ptr<FsNode> getTarget() override {
		return _target;
	}

public:
	PeripheralLink(std::shared_ptr<FsNode> owner,
			std::string name, std::shared_ptr<FsNode> target)
	: Link{std::move(owner), std::move(name)}, _target{std::move(target)} { }

private:
	std::string _name;
	std::shared_ptr<FsNode> _target;
};

// This class is embedded in a DirectoryNode and share its lifetime.
struct StructuralLink final : Link {
private:
	std::shared_ptr<FsNode> getTarget() override;

public:
	StructuralLink(DirectoryNode *target)
	: _target{std::move(target)} {
		assert(_target);
	}

	StructuralLink(std::shared_ptr<FsNode> owner, DirectoryNode *target, std::string name)
	: Link{std::move(owner), std::move(name)}, _target{std::move(target)} {
		assert(_target);
	}

private:
	DirectoryNode *_target;
};

struct DirectoryNode final : Node {
private:
	VfsType getType() override {
		return VfsType::directory;
	}

	std::shared_ptr<FsLink> treeLink() override {
		auto self = std::shared_ptr<FsNode>{weakNode()};
		return std::shared_ptr<FsLink>{std::move(self), &_treeLink};
	}

	async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	mkdir(std::string name) override {
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::PullDescriptor pull_node;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_MKDIR);
		req.set_path(name);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(getLane(), helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&pull_node));
		co_await transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pull_node.error());

			auto child = _sb->internalizeStructural(this, name,
					resp.id(), pull_node.descriptor());
			co_return child->treeLink();
		} else {
			co_return Error::illegalOperationTarget; // TODO
		}
	}

	FutureMaybe<std::shared_ptr<FsLink>> symlink(std::string name, std::string link) override {
		(void)name;
		(void)link;
		assert(!"symlink is not implemented for extern_fs");
		__builtin_unreachable();
	}

	FutureMaybe<std::shared_ptr<FsLink>> mkdev(std::string name, VfsType type, DeviceId id) override {
		(void)name;
		(void)type;
		(void)id;
		assert(!"mkdev is not implemented for extern_fs");
		__builtin_unreachable();
	}

	FutureMaybe<std::shared_ptr<FsLink>>
			getLink(std::string name) override {
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::PullDescriptor pull_node;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_GET_LINK);
		req.set_path(name);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(getLane(), helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&pull_node));
		co_await transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pull_node.error());

			if(resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				auto child = _sb->internalizeStructural(this, name,
						resp.id(), pull_node.descriptor());
				co_return child->treeLink();
			}else{
				auto child = _sb->internalizePeripheralNode(resp.file_type(), resp.id(),
						pull_node.descriptor());
				co_return _sb->internalizePeripheralLink(this, name, std::move(child));
			}
		}else{
			co_return nullptr;
		}
	}

	FutureMaybe<std::shared_ptr<FsLink>> link(std::string name,
			std::shared_ptr<FsNode> target) override {
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::PullDescriptor pull_node;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_LINK);
		req.set_path(name);
		req.set_fd(static_cast<Node *>(target.get())->getInode());

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(getLane(), helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&pull_node));
		co_await transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pull_node.error());

			if(resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				auto child = _sb->internalizeStructural(this, name,
						resp.id(), pull_node.descriptor());
				co_return child->treeLink();
			}else{
				auto child = _sb->internalizePeripheralNode(resp.file_type(), resp.id(),
						pull_node.descriptor());
				co_return _sb->internalizePeripheralLink(this, name, std::move(child));
			}
		}else{
			co_return nullptr;
		}
	}

	FutureMaybe<void> unlink(std::string name) override {
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_UNLINK);
		req.set_path(name);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(getLane(), helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp));
		co_await transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::fs::Errors::SUCCESS);
	}

	FutureMaybe<SharedFilePtr>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		// Regular files do not support O_NONBLOCK.
		semantic_flags &= ~semanticNonBlock;

		assert(!semantic_flags);
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::PullDescriptor pull_ctrl;
		helix::PullDescriptor pull_passthrough;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_OPEN);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(getLane(), helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&pull_ctrl, kHelItemChain),
				helix::action(&pull_passthrough));
		co_await transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(pull_ctrl.error());
		HEL_CHECK(pull_passthrough.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		auto file = smarter::make_shared<OpenFile>(pull_ctrl.descriptor(),
				pull_passthrough.descriptor(), std::move(mount), std::move(link));
		file->setupWeakFile(file);
		co_return File::constructHandle(std::move(file));
	}

public:
	DirectoryNode(Superblock *sb, uint64_t inode, helix::UniqueLane lane)
	: Node{inode, std::move(lane), sb}, _sb{sb},
			_treeLink{this} { }

	DirectoryNode(Superblock *sb, std::shared_ptr<Node> owner, std::string name,
			uint64_t inode, helix::UniqueLane lane)
	: Node{inode, std::move(lane), sb}, _sb{sb},
			_treeLink{std::move(owner), this, std::move(name)} { }

private:
	Superblock *_sb;
	StructuralLink _treeLink;
};

std::shared_ptr<FsNode> StructuralLink::getTarget() {
	return std::shared_ptr<Node>{_target->weakNode()};
}

Superblock::Superblock(helix::UniqueLane lane)
: _lane{std::move(lane)} { }

FutureMaybe<std::shared_ptr<FsNode>> Superblock::createRegular() {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;
	helix::PullDescriptor pull_node;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::SB_CREATE_REGULAR);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&pull_node));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	if(resp.error() == managarm::fs::Errors::SUCCESS) {
		HEL_CHECK(pull_node.error());

		co_return internalizePeripheralNode(resp.file_type(), resp.id(),
				pull_node.descriptor());
	}else{
		co_return nullptr;
	}
}

FutureMaybe<std::shared_ptr<FsNode>> Superblock::createSocket() {
	throw std::runtime_error("extern_fs: createSocket() is not supported");
}

async::result<std::shared_ptr<FsLink>> Superblock::rename(FsLink *source,
		FsNode *directory, std::string name) {
	throw std::runtime_error("extern_fs: rename() is not supported");
}

std::shared_ptr<Node> Superblock::internalizeStructural(uint64_t id, helix::UniqueLane lane) {
	auto entry = &_activeStructural[id];
	auto intern = entry->lock();
	if(intern)
		return intern;

	auto node = std::make_shared<DirectoryNode>(this, id, std::move(lane));
	node->setupWeakNode(node);
	*entry = node;
	return node;
}

std::shared_ptr<Node> Superblock::internalizeStructural(Node *parent, std::string name,
		uint64_t id, helix::UniqueLane lane) {
	auto entry = &_activeStructural[id];
	auto intern = entry->lock();
	if(intern)
		return intern;

	auto owner = std::shared_ptr<Node>{parent->weakNode()};
	auto node = std::make_shared<DirectoryNode>(this, owner, std::move(name), id, std::move(lane));
	node->setupWeakNode(node);
	*entry = node;
	return node;
}

std::shared_ptr<Node> Superblock::internalizePeripheralNode(int64_t type,
		int id, helix::UniqueLane lane) {
	auto entry = &_activePeripheralNodes[id];
	auto intern = entry->lock();
	if(intern)
		return intern;

	std::shared_ptr<Node> node;
	switch(type) {
	case managarm::fs::FileType::REGULAR:
		node = std::make_shared<RegularNode>(this, id, std::move(lane));
		break;
	case managarm::fs::FileType::SYMLINK:
		node = std::make_shared<SymlinkNode>(id, std::move(lane));
		break;
	default:
		throw std::runtime_error("extern_fs: Unexpected file type");
	}
	node->setupWeakNode(node);
	*entry = node;
	return node;
}

std::shared_ptr<FsLink> Superblock::internalizePeripheralLink(Node *parent, std::string name,
		std::shared_ptr<Node> target) {
	auto entry = &_activePeripheralLinks[{parent, name}];
	auto intern = entry->lock();
	if(intern)
		return intern;

	auto owner = std::shared_ptr<Node>{parent->weakNode()};
	auto link = std::make_shared<PeripheralLink>(std::move(owner),
			std::move(name), std::move(target));
	*entry = link;
	return link;
}

} // anonymous namespace

std::shared_ptr<FsLink> createRoot(helix::UniqueLane sb_lane, helix::UniqueLane lane) {
	auto sb = new Superblock{std::move(sb_lane)};
	// FIXME: 2 is the ext2fs root inode.
	auto node = sb->internalizeStructural(2, std::move(lane));
	return node->treeLink();
}

smarter::shared_ptr<File, FileHandle>
createFile(helix::UniqueLane lane, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link) {
	auto file = smarter::make_shared<OpenFile>(helix::UniqueLane{},
			std::move(lane), std::move(mount), std::move(link));
	file->setupWeakFile(file);
	return File::constructHandle(std::move(file));
}

} // namespace extern_fs

