#include <async/cancellation.hpp>
#include <sys/epoll.h>
#include <map>

#include <frg/std_compat.hpp>
#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "extern_fs.hpp"
#include "process.hpp"
#include "fs.bragi.hpp"

#include <bitset>

namespace extern_fs {

namespace {

struct Node;
struct DirectoryNode;

struct Superblock final : FsSuperblock {
	Superblock(helix::UniqueLane lane, std::shared_ptr<UnixDevice> device);

	FutureMaybe<std::shared_ptr<FsNode>> createRegular(Process *process) override;
	FutureMaybe<std::shared_ptr<FsNode>> createSocket() override;

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
			rename(FsLink *source, FsNode *directory, std::string name) override;
	async::result<frg::expected<Error, FsFileStats>> getFsstats() override;

	std::string getFsType() override {
		return "ext2";
	}

	dev_t deviceNumber() override {
		auto id = device_->getId();
		return makedev(id.first, id.second);
	}

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
	std::map<std::tuple<uint64_t, std::string, uint64_t>, std::weak_ptr<FsLink>> _activePeripheralLinks;

	std::shared_ptr<UnixDevice> device_;
};

struct Node : FsNode {
	async::result<frg::expected<Error, FileStats>> getStats() override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_GET_STATS);

		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		FileStats stats{};
		stats.inodeNumber = getInode(); // TODO: Move this out of FileStats.
		stats.fileSize = resp.file_size();
		stats.numLinks = resp.num_links();
		stats.mode = resp.mode();
		stats.uid = resp.uid();
		stats.gid = resp.gid();
		stats.atimeSecs = resp.atime_secs();
		stats.atimeNanos = resp.atime_nanos();
		stats.mtimeSecs = resp.mtime_secs();
		stats.mtimeNanos = resp.mtime_nanos();
		stats.ctimeSecs = resp.ctime_secs();
		stats.ctimeNanos = resp.ctime_nanos();

		co_return stats;
	}

	async::result<Error> chmod(int mode) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_CHMOD);
		req.set_mode(mode);

		auto ser = req.SerializeAsString();
		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		co_return Error::success;
	}

	async::result<Error> utimensat(std::optional<timespec> atime, std::optional<timespec> mtime,
			timespec ctime) override {
		managarm::fs::UtimensatRequest req;
		if(atime) {
			req.set_atime_sec(atime->tv_sec);
			req.set_atime_nsec(atime->tv_nsec);
			req.set_atime_update(true);
		}

		if(mtime) {
			req.set_mtime_sec(mtime->tv_sec);
			req.set_mtime_nsec(mtime->tv_nsec);
			req.set_mtime_update(true);
		}

		req.set_ctime_sec(ctime.tv_sec);
		req.set_ctime_nsec(ctime.tv_nsec);

		auto ser = req.SerializeAsString();
		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		co_return Error::success;
	}


public:
	Node(uint64_t inode, helix::UniqueLane lane, Superblock *sb)
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
	async::result<frg::expected<Error, off_t>> seek(off_t offset, VfsSeek whence) override {
		if(whence == VfsSeek::absolute) {
			co_await _file.seekAbsolute(offset);
			co_return offset;
		} else if(whence == VfsSeek::relative) {
			co_return co_await _file.seekRelative(offset);
		} else if(whence == VfsSeek::eof) {
			co_return co_await _file.seekEof(offset);
		}
		co_return Error::illegalArguments;
	}

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override {
		size_t res = co_await _file.writeSome(data, length);
		co_return res;
	}

	// TODO: Ensure that the process is null? Pass credentials of the thread in the request?
	async::result<std::expected<size_t, Error>>
	readSome(Process *, void *data, size_t max_length, async::cancellation_token ce) override {
		auto res = co_await _file.readSome(data, max_length, ce);
		co_return res.transform_error(toPosixError);
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
			async::cancellation_token cancellation) override {
		(void)mask;

		if(sequence > 1)
			co_return Error::illegalArguments;

		if(sequence)
			co_await async::suspend_indefinitely(cancellation);
		co_return PollWaitResult{1, EPOLLIN | EPOLLOUT};
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		co_return PollStatusResult{1, EPOLLIN | EPOLLOUT};
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
			std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, bool append)
	: File{FileKind::unknown, StructName::get("externfs.file"), std::move(mount), std::move(link), 0, append},
			_control{std::move(control)}, _file{std::move(lane)} { }

	~OpenFile() override {
		// It's not necessary to do any cleanup here.
	}

	void handleClose() override {
		// Close the control lane to inform the server that we closed the file.
		_control = helix::UniqueLane{};
	}

	async::result<frg::expected<protocols::fs::Error>> truncate(size_t size) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::PT_TRUNCATE);
		req.set_size(size);

		auto ser = req.SerializeAsString();
		auto [offer, send_req, recv_resp]
				= co_await helix_ng::exchangeMsgs(getPassthroughLane(),
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		assert(resp.error() == managarm::fs::Errors::SUCCESS);
		co_return {};
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

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		// Regular files do not support O_NONBLOCK.
		semantic_flags &= ~semanticNonBlock;

		if(semantic_flags & ~(semanticRead | semanticWrite | semanticAppend)){
			std::cout << "\e[31mposix: extern_fs OpenFile open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2), semanticWrite (0x4) and semanticAppend (0x8) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		bool append = false;
		if(semantic_flags & semanticAppend) {
			append = true;
		}

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_OPEN);
		req.set_append(append);

		auto [offer, send_req, recv_resp, pull_ctrl, pull_passthrough] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(),
				helix_ng::pullDescriptor()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(pull_ctrl.error());
		HEL_CHECK(pull_passthrough.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		auto file = smarter::make_shared<OpenFile>(pull_ctrl.descriptor(),
				pull_passthrough.descriptor(), std::move(mount), std::move(link), append);
		file->setupWeakFile(file);
		co_return File::constructHandle(std::move(file));
	}

public:
	RegularNode(Superblock *sb, uint64_t inode, helix::UniqueLane lane)
	: Node{inode, std::move(lane), sb} { }
};

struct SymlinkNode final : Node {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	expected<std::string> readSymlink(FsLink *, Process *) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_READ_SYMLINK);

		auto [offer, send_req, recv_resp, recv_target] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(recv_target.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		co_return std::string{static_cast<char *>(recv_target.data()), recv_target.length()};
	}

public:
	SymlinkNode(Superblock *sb, uint64_t inode, helix::UniqueLane lane)
	: Node{inode, std::move(lane), sb} { }
};

struct Link : FsLink {
public:
	std::shared_ptr<FsNode> getOwner() override {
		return _owner;
	}

	async::result<frg::expected<Error>> obstruct() override {
		assert(_owner);
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_OBSTRUCT_LINK);
		req.set_link_name(_name);

		auto lane = static_cast<Node *>(_owner.get())->getLane();

		auto ser = req.SerializeAsString();
		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		assert(resp.error() == managarm::fs::Errors::SUCCESS);
		co_return frg::success_tag{};
	}

private:
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

	bool hasTraverseLinks() override {
		return true;
	}

	async::result<std::expected<std::shared_ptr<FsLink>, Error>>
	getLinkOrCreate(Process *process, std::string name, mode_t mode, bool exclusive) override {
		assert(this->getType() == VfsType::directory);

		managarm::fs::GetLinkOrCreateRequest req;
		req.set_mode(mode);
		req.set_exclusive(exclusive);
		req.set_name(name);
		req.set_uid(process->uid());
		req.set_gid(process->gid());

		auto [offer, send_head, send_tail, recv_resp, pull_node] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_head.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GetLinkOrCreateResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pull_node.error());

			auto node = _sb->internalizePeripheralNode(resp.file_type(), resp.id(), pull_node.descriptor());
			co_return _sb->internalizePeripheralLink(this, name, std::move(node));
		} else {
			co_return std::unexpected{resp.error() | toPosixError};
		}
	}

	async::result<frg::expected<Error, std::pair<std::shared_ptr<FsLink>, size_t>>>
	traverseLinks(std::deque<std::string> path) override {
		managarm::fs::NodeTraverseLinksRequest req;
		for (auto &i : path)
			req.add_path_segments(i);

		auto [offer, send_head, send_tail, recv_resp, pull_desc] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor()
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(send_head.error());
		HEL_CHECK(send_tail.error());
		HEL_CHECK(recv_resp.error());

		std::shared_ptr<FsLink> link = nullptr;

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();

		if (resp.error() == managarm::fs::Errors::FILE_NOT_FOUND) {
			co_return Error::noSuchFile;
		} else if (resp.error() == managarm::fs::Errors::NOT_DIRECTORY) {
			co_return Error::notDirectory;
		} else {
			assert(resp.error() == managarm::fs::Errors::SUCCESS);
			HEL_CHECK(pull_desc.error());
		}

		helix::UniqueLane pull_lane = pull_desc.descriptor();

		assert(resp.links_traversed());
		assert(resp.links_traversed() <= path.size());

		std::shared_ptr<Node> parentNode{weakNode()};
		for (size_t i = 0; i < resp.ids().size(); i++) {
			auto [pull_node] = co_await helix_ng::exchangeMsgs(
				pull_lane,
				helix_ng::pullDescriptor()
			);

			HEL_CHECK(pull_node.error());

			if (i != resp.ids().size() - 1
					|| resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				auto child = _sb->internalizeStructural(parentNode.get(), path[i],
						resp.ids()[i], pull_node.descriptor());
				if (i != resp.ids().size() - 1)
					parentNode = child;
				else
					link = child->treeLink();
			}else{
				auto child = _sb->internalizePeripheralNode(resp.file_type(), resp.ids()[i],
						pull_node.descriptor());
				link = _sb->internalizePeripheralLink(parentNode.get(), path[i], std::move(child));
			}
		}

		co_return std::make_pair(link, resp.links_traversed());
	}

	async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	mkdir(std::string name) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_MKDIR);
		req.set_path(name);

		auto ser = req.SerializeAsString();
		auto [offer, sendReq, recvResp, pullNode] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(recvResp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recvResp.data(), recvResp.length());
		recvResp.reset();
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pullNode.error());

			auto child = _sb->internalizeStructural(this, name,
					resp.id(), pullNode.descriptor());
			co_return child->treeLink();
		} else {
			co_return Error::illegalOperationTarget; // TODO
		}
	}

	async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	symlink(std::string name, std::string path) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_SYMLINK);
		req.set_name_length(name.size());
		req.set_target_length(path.size());

		auto ser = req.SerializeAsString();
		auto [offer, sendReq, sendName, sendTarget, recvResp, pullNode]
			= co_await helix_ng::exchangeMsgs(getLane(),
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(name.data(), name.size()),
				helix_ng::sendBuffer(path.data(), path.size()),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(sendName.error());
		HEL_CHECK(sendTarget.error());
		HEL_CHECK(recvResp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recvResp.data(), recvResp.length());
		recvResp.reset();
		if(resp.error() == managarm::fs::Errors::SUCCESS) {
			HEL_CHECK(pullNode.error());

			auto child = _sb->internalizeStructural(this, name,
					resp.id(), pullNode.descriptor());
			co_return child->treeLink();
		} else {
			co_return Error::illegalOperationTarget; // TODO
		}
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> mkdev(std::string name, VfsType type, DeviceId id) override {
		(void)name;
		(void)type;
		(void)id;
		assert(!"mkdev is not implemented for extern_fs");
		__builtin_unreachable();
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
			getLink(std::string name) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_GET_LINK);
		req.set_path(name);

		auto [offer, send_req, recv_resp, pull_node] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
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
		}else if(resp.error() == managarm::fs::Errors::FILE_NOT_FOUND) {
			co_return Error::noSuchFile;
		}else{
			assert(resp.error() == managarm::fs::Errors::NOT_DIRECTORY);
			co_return Error::notDirectory;
		}
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> link(std::string name,
			std::shared_ptr<FsNode> target) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_LINK);
		req.set_path(name);
		req.set_fd(static_cast<Node *>(target.get())->getInode());

		auto [offer, send_req, recv_resp, pull_node] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
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

	async::result<frg::expected<Error>> unlink(std::string name) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_UNLINK);
		req.set_path(name);

		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		if(resp.error() == managarm::fs::Errors::FILE_NOT_FOUND)
			co_return Error::noSuchFile;
		else if(resp.error() == managarm::fs::Errors::DIRECTORY_NOT_EMPTY)
			co_return Error::directoryNotEmpty;
		assert(resp.error() == managarm::fs::Errors::SUCCESS);
		co_return {};
	}

	async::result<frg::expected<Error>> rmdir(std::string name) override {
		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_RMDIR);
		req.set_path(name);

		auto ser = req.SerializeAsString();
		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();

		if(resp.error() == managarm::fs::Errors::DIRECTORY_NOT_EMPTY) {
			co_return Error::directoryNotEmpty;
		}

		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		co_return {};
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		// Regular files do not support O_NONBLOCK.
		semantic_flags &= ~semanticNonBlock;

		if(semantic_flags & ~(semanticRead | semanticWrite | semanticAppend)){
			std::cout << "\e[31mposix: extern_fs DirectoryNode open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2), semanticWrite (0x4) and semanticAppend (0x8) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		bool append = false;
		if(semantic_flags & semanticAppend) {
			append = true;
		}

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_OPEN);
		req.set_append(append);

		auto [offer, send_req, recv_resp, pull_ctrl, pull_passthrough] = co_await helix_ng::exchangeMsgs(
			getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor(),
				helix_ng::pullDescriptor()
			)
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(pull_ctrl.error());
		HEL_CHECK(pull_passthrough.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		recv_resp.reset();
		assert(resp.error() == managarm::fs::Errors::SUCCESS);

		auto file = smarter::make_shared<OpenFile>(pull_ctrl.descriptor(),
				pull_passthrough.descriptor(), std::move(mount), std::move(link), append);
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

Superblock::Superblock(helix::UniqueLane lane, std::shared_ptr<UnixDevice> device)
: _lane{std::move(lane)}, device_{device} { }

FutureMaybe<std::shared_ptr<FsNode>> Superblock::createRegular(Process *process) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::SB_CREATE_REGULAR);
	req.set_uid(process->uid());
	req.set_gid(process->gid());

	auto [offer, send_req, recv_resp, pull_node] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	recv_resp.reset();
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

async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
		Superblock::rename(FsLink *source, FsNode *directory, std::string name) {

	managarm::fs::RenameRequest req;
	Link *slink = static_cast<Link *>(source);
	Node *source_node = static_cast<Node *>(slink->getOwner().get());
	Node *target_node = static_cast<Node *>(directory);
	std::shared_ptr<Node> shared_node = std::static_pointer_cast<Node>(source->getTarget());
	req.set_inode_source(source_node->getInode());
	req.set_inode_target(target_node->getInode());
	req.set_old_name(source->getName());
	req.set_new_name(name);

	auto [offer, send_head, send_tail, recv_resp] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
			helix_ng::recvInline()
		)
	);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_head.error());
	HEL_CHECK(send_tail.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	recv_resp.reset();
	if(resp.error() == managarm::fs::Errors::SUCCESS) {
		co_return internalizePeripheralLink(target_node, name, shared_node);
	}else{
		co_return nullptr;
	}
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
		node = std::make_shared<SymlinkNode>(this, id, std::move(lane));
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
	auto entry = &_activePeripheralLinks[{parent->getInode(), name, target->getInode()}];
	auto intern = entry->lock();
	if(intern)
		return intern;

	auto owner = std::shared_ptr<Node>{parent->weakNode()};
	auto link = std::make_shared<PeripheralLink>(std::move(owner),
			std::move(name), std::move(target));
	*entry = link;
	return link;
}

async::result<frg::expected<Error, FsFileStats>> Superblock::getFsstats() {
	std::cout << "posix: unimplemented getFsstats for extern_fs Superblock!" << std::endl;
	co_return Error::illegalOperationTarget;
}

} // anonymous namespace

std::shared_ptr<FsLink> createRoot(helix::UniqueLane sb_lane, helix::UniqueLane lane, std::shared_ptr<UnixDevice> device) {
	auto sb = new Superblock{std::move(sb_lane), device};
	// FIXME: 2 is the ext2fs root inode.
	auto node = sb->internalizeStructural(2, std::move(lane));
	return node->treeLink();
}

smarter::shared_ptr<File, FileHandle>
createFile(helix::UniqueLane lane, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link) {
	auto file = smarter::make_shared<OpenFile>(helix::UniqueLane{},
			std::move(lane), std::move(mount), std::move(link), false);
	file->setupWeakFile(file);
	return File::constructHandle(std::move(file));
}

} // namespace extern_fs

