
#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "extern_fs.hpp"
#include "fs.pb.h"

namespace extern_fs {

namespace {

struct Node : FsNode {
	void setupWeakNode(std::weak_ptr<Node> self) {
		_self = std::move(self);
	}

	std::weak_ptr<Node> weakNode() {
		return _self;
	}

private:
	std::weak_ptr<Node> _self;
};

struct DirectoryNode;

struct Context {
	std::shared_ptr<Node> internalizeStructural(int64_t id, helix::UniqueLane lane);
	std::shared_ptr<Node> internalizeStructural(Node *owner, int64_t id, helix::UniqueLane lane);
	std::shared_ptr<Node> internalizePeripheralNode(int64_t type, int id, helix::UniqueLane lane);
	std::shared_ptr<FsLink> internalizePeripheralLink(Node *parent, std::string name,
			std::shared_ptr<Node> target);

private:
	std::map<int64_t, std::weak_ptr<DirectoryNode>> _activeStructural;
	std::map<int64_t, std::weak_ptr<Node>> _activePeripheralNodes;
	std::map<std::pair<Node *, std::string>, std::weak_ptr<FsLink>> _activePeripheralLinks;
};

struct OpenFile : File {
private:
	COFIBER_ROUTINE(expected<off_t>, seek(off_t offset, VfsSeek whence) override, ([=] {
		assert(whence == VfsSeek::absolute);
		COFIBER_AWAIT _file.seekAbsolute(offset);
		COFIBER_RETURN(offset);
	}))

	// TODO: Ensure that the process is null? Pass credentials of the thread in the request?
	COFIBER_ROUTINE(expected<size_t>,
	readSome(Process *, void *data, size_t max_length) override, ([=] {
		size_t length = COFIBER_AWAIT _file.readSome(data, max_length);
		COFIBER_RETURN(length);
	}))
	
	// TODO: For extern_fs, we can simply return POLLIN | POLLOUT here.
	// Move device code out of this file.
	COFIBER_ROUTINE(expected<PollResult>, poll(Process *, uint64_t sequence) override, ([=] {
		auto result = COFIBER_AWAIT _file.poll(sequence);
		COFIBER_RETURN(result);
	}))

	COFIBER_ROUTINE(FutureMaybe<helix::UniqueDescriptor>, accessMemory(off_t offset) override, ([=] {
		auto memory = COFIBER_AWAIT _file.accessMemory(offset);
		COFIBER_RETURN(std::move(memory));
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _file.getLane();
	}

public:
	OpenFile(helix::UniqueLane control, helix::UniqueLane lane, std::shared_ptr<FsLink> link)
	: File{StructName::get("externfs.file"), std::move(link)},
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

struct RegularNode : Node {
private:
	VfsType getType() override {
		return VfsType::regular;
	}

	COFIBER_ROUTINE(async::result<FileStats>, getStats() override, ([=] {
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_GET_STATS);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		FileStats stats{};
		stats.inodeNumber = _inode; // TODO: Move this out of FileStats.
		stats.fileSize = resp.file_size();
		stats.numLinks = resp.num_links();
		stats.mode = resp.mode();
		stats.uid = resp.uid();
		stats.gid = resp.gid();
		stats.atimeSecs = resp.atime_secs();
		stats.atimeNanos = resp.atime_nanos();
		stats.ctimeSecs = resp.mtime_secs();
		stats.atimeNanos = resp.mtime_nanos();
		stats.ctimeSecs = resp.ctime_secs();
		stats.atimeNanos = resp.ctime_nanos();

		COFIBER_RETURN(stats);
	}))

	COFIBER_ROUTINE(FutureMaybe<SharedFilePtr>,
			open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override, ([=] {
		assert(!semantic_flags);
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::PullDescriptor pull_ctrl;
		helix::PullDescriptor pull_passthrough;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_OPEN);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&pull_ctrl, kHelItemChain),
				helix::action(&pull_passthrough));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(pull_ctrl.error());
		HEL_CHECK(pull_passthrough.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		auto file = smarter::make_shared<OpenFile>(pull_ctrl.descriptor(),
				pull_passthrough.descriptor(), std::move(link));
		file->setupWeakFile(file);
		COFIBER_RETURN(File::constructHandle(std::move(file)));
	}))

public:
	RegularNode(uint64_t inode, helix::UniqueLane lane)
	: _inode{inode}, _lane{std::move(lane)} { }

private:
	uint64_t _inode;
	helix::UniqueLane _lane;
};

struct SymlinkNode : Node {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		FileStats stats{};
		stats.inodeNumber = _inode; // TODO: Move this out of FileStats.
		std::cout << "\e[31mposix: Fix extern_fs SymlinkNode::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(stats);
	}))

	COFIBER_ROUTINE(expected<std::string>, readSymlink(FsLink *) override, ([=] {
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::RecvInline recv_target;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_READ_SYMLINK);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&recv_target));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(recv_target.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::fs::Errors::SUCCESS);
		
		COFIBER_RETURN((std::string{static_cast<char *>(recv_target.data()), recv_target.length()}));
	}))

public:
	SymlinkNode(int64_t inode, helix::UniqueLane lane)
	: _inode{inode}, _lane{std::move(lane)} { }

private:
	int64_t _inode;
	helix::UniqueLane _lane;
};

struct Link : FsLink {
private:
	std::shared_ptr<FsNode> getOwner() override {
		return _owner;
	}

	std::string getName() override {
		assert(!"No associated name");
	}

public:
	Link() = default;

	Link(std::shared_ptr<FsNode> owner)
	: _owner{std::move(owner)} { }

private:
	std::shared_ptr<FsNode> _owner;
};

// This class maintains a strong reference to the target.
struct PeripheralLink : Link {
private:
	std::shared_ptr<FsNode> getTarget() override {
		return _target;
	}

public:
	PeripheralLink(std::shared_ptr<FsNode> owner, std::shared_ptr<FsNode> target)
	: Link{std::move(owner)}, _target{std::move(target)} { }

private:
	std::shared_ptr<FsNode> _target;
};

// This class is embedded in a DirectoryNode and share its lifetime.
struct StructuralLink : Link {
private:
	std::shared_ptr<FsNode> getTarget() override;

public:
	StructuralLink(DirectoryNode *target)
	: _target{std::move(target)} {
		assert(_target);
	}

	StructuralLink(std::shared_ptr<FsNode> owner, DirectoryNode *target)
	: Link{std::move(owner)}, _target{std::move(target)} {
		assert(_target);
	}

private:
	DirectoryNode *_target;
};

struct DirectoryNode : Node {
private:
	VfsType getType() override {
		return VfsType::directory;
	}

	std::shared_ptr<FsLink> treeLink() {
		auto self = std::shared_ptr<FsNode>{weakNode()};
		return std::shared_ptr<FsLink>{std::move(self), &_treeLink};
	}

	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		FileStats stats{};
		stats.inodeNumber = _inode; // TODO: Move this out of FileStats.
		std::cout << "\e[31mposix: Fix extern_fs DirectoryNode::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(stats);
	}))

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>,
			mkdir(std::string name) override, ([=] {
		(void)name;
		assert(!"mkdir is not implemented for extern_fs");
	}))
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>,
			symlink(std::string name, std::string link) override, ([=] {
		(void)name;
		(void)link;
		assert(!"symlink is not implemented for extern_fs");
	}))
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>, mkdev(std::string name,
			VfsType type, DeviceId id) override, ([=] {
		(void)name;
		(void)type;
		(void)id;
		assert(!"mkdev is not implemented for extern_fs");
	}))

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>,
			getLink(std::string name) override, ([=] {
//		std::cout << "extern_fs: getLink() " << name << std::endl;

		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::PullDescriptor pull_node;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_GET_LINK);
		req.set_path(name);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&pull_node));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pull_node.error());

			if(resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				auto child = _context->internalizeStructural(this,
						resp.id(), pull_node.descriptor());
				COFIBER_RETURN(child->treeLink());
			}else{
				auto child = _context->internalizePeripheralNode(resp.file_type(), resp.id(),
						pull_node.descriptor());
				COFIBER_RETURN(_context->internalizePeripheralLink(this, name, std::move(child)));
			}
		}else{
			COFIBER_RETURN(nullptr);
		}
	}))

	COFIBER_ROUTINE(FutureMaybe<SharedFilePtr>,
			open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override, ([=] {
		assert(!semantic_flags);
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::PullDescriptor pull_ctrl;
		helix::PullDescriptor pull_passthrough;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_OPEN);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&pull_ctrl, kHelItemChain),
				helix::action(&pull_passthrough));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(pull_ctrl.error());
		HEL_CHECK(pull_passthrough.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		auto file = smarter::make_shared<OpenFile>(pull_ctrl.descriptor(),
				pull_passthrough.descriptor(), std::move(link));
		file->setupWeakFile(file);
		COFIBER_RETURN(File::constructHandle(std::move(file)));
	}))

public:
	DirectoryNode(Context *context, int64_t inode, helix::UniqueLane lane)
	: _context{context}, _treeLink{this}, _inode{inode}, _lane{std::move(lane)} { }

	DirectoryNode(Context *context, std::shared_ptr<Node> owner,
			int64_t inode, helix::UniqueLane lane)
	: _context{context}, _treeLink{std::move(owner), this},
			_inode{inode}, _lane{std::move(lane)} { }

private:
	Context *_context;
	StructuralLink _treeLink;
	int64_t _inode;
	helix::UniqueLane _lane;
};

std::shared_ptr<FsNode> StructuralLink::getTarget() {
	return std::shared_ptr<Node>{_target->weakNode()};
}

std::shared_ptr<Node> Context::internalizeStructural(int64_t id, helix::UniqueLane lane) {
	auto entry = &_activeStructural[id];
	auto intern = entry->lock();
	if(intern)
		return std::move(intern);

	auto node = std::make_shared<DirectoryNode>(this, id, std::move(lane));
	node->setupWeakNode(node);
	*entry = node;
	return std::move(node);
}

std::shared_ptr<Node> Context::internalizeStructural(Node *parent,
		int64_t id, helix::UniqueLane lane) {
	auto entry = &_activeStructural[id];
	auto intern = entry->lock();
	if(intern)
		return std::move(intern);

	auto owner = std::shared_ptr<Node>{parent->weakNode()};
	auto node = std::make_shared<DirectoryNode>(this, owner, id, std::move(lane));
	node->setupWeakNode(node);
	*entry = node;
	return std::move(node);
}

std::shared_ptr<Node> Context::internalizePeripheralNode(int64_t type,
		int id, helix::UniqueLane lane) {
	auto entry = &_activePeripheralNodes[id];
	auto intern = entry->lock();
	if(intern)
		return std::move(intern);

	std::shared_ptr<Node> node;
	switch(type) {
	case managarm::fs::FileType::REGULAR:
		node = std::make_shared<RegularNode>(id, std::move(lane));
		break;
	case managarm::fs::FileType::SYMLINK:
		node = std::make_shared<SymlinkNode>(id, std::move(lane));
		break;
	default:
		throw std::runtime_error("extern_fs: Unexpected file type");
	}
	node->setupWeakNode(node);
	*entry = node;
	return std::move(node);
}

std::shared_ptr<FsLink> Context::internalizePeripheralLink(Node *parent, std::string name,
		std::shared_ptr<Node> target) {
	auto entry = &_activePeripheralLinks[{parent, name}];
	auto intern = entry->lock();
	if(intern)
		return std::move(intern);

	auto owner = std::shared_ptr<Node>{parent->weakNode()};
	auto link = std::make_shared<PeripheralLink>(std::move(owner), std::move(target));
	*entry = link;
	return link;
}

} // anonymous namespace

std::shared_ptr<FsLink> createRoot(helix::UniqueLane lane) {
	auto context = new Context{};
	// FIXME: 2 is the ext2fs root inode.
	auto node = context->internalizeStructural(2, std::move(lane));
	return node->treeLink();
}

smarter::shared_ptr<File, FileHandle>
createFile(helix::UniqueLane lane, std::shared_ptr<FsLink> link) {
	auto file = smarter::make_shared<OpenFile>(helix::UniqueLane{},
			std::move(lane), std::move(link));
	file->setupWeakFile(file);
	return File::constructHandle(std::move(file));
}

} // namespace extern_fs

