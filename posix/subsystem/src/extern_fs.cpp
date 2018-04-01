
#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "extern_fs.hpp"
#include "fs.pb.h"

namespace extern_fs {

namespace {

struct Context {

	std::shared_ptr<FsNode> internalizeNode(int64_t id, std::shared_ptr<FsNode> node);
	std::shared_ptr<FsLink> internalizeLink(FsNode *parent, std::string name,
			std::shared_ptr<FsLink> link);

private:
	std::map<int64_t, std::weak_ptr<FsNode>> _activeNodes;
	std::map<std::pair<FsNode *, std::string>, std::weak_ptr<FsLink>> _activeLinks;
};

struct OpenFile : File {
private:
	COFIBER_ROUTINE(FutureMaybe<off_t>, seek(off_t offset, VfsSeek whence) override, ([=] {
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
	COFIBER_ROUTINE(expected<PollResult>, poll(uint64_t sequence) override, ([=] {
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

	void handleClose() override {
		// Close the control lane to inform the server that we closed the file.
		_control = helix::UniqueLane{};
	}

private:
	helix::UniqueLane _control;
	protocols::fs::File _file;
};

struct RegularNode : FsNode {
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

		FileStats stats;
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
		COFIBER_RETURN(File::constructHandle(std::move(file)));
	}))

public:
	RegularNode(uint64_t inode, helix::UniqueLane lane)
	: _inode{inode}, _lane{std::move(lane)} { }

private:
	uint64_t _inode;
	helix::UniqueLane _lane;
};

struct SymlinkNode : FsNode {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	FutureMaybe<FileStats> getStats() override {
		throw std::runtime_error("extern_fs: Fix SymlinkNode::getStats()");
	}

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
	SymlinkNode(helix::UniqueLane lane)
	: _lane{std::move(lane)} { }

private:
	helix::UniqueLane _lane;
};

struct Entry : FsLink {
private:
	std::shared_ptr<FsNode> getOwner() override {
		return _owner;
	}

	std::string getName() override {
		assert(!"No associated name");
	}

	std::shared_ptr<FsNode> getTarget() override {
		return _target;
	}

public:
	Entry(std::shared_ptr<FsNode> owner, std::shared_ptr<FsNode> target)
	: _owner{std::move(owner)}, _target{std::move(target)} { }

private:
	std::shared_ptr<FsNode> _owner;
	std::shared_ptr<FsNode> _target;
};

struct DirectoryNode : FsNode, std::enable_shared_from_this<DirectoryNode> {
private:
	VfsType getType() override {
		return VfsType::directory;
	}

	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		std::cout << "\e[31mposix: Fix externfs DirectoryNode::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(FileStats{});
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

			std::shared_ptr<FsNode> child;
			switch(resp.file_type()) {
			case managarm::fs::FileType::DIRECTORY:
				child = std::make_shared<DirectoryNode>(_context, pull_node.descriptor());
				break;
			case managarm::fs::FileType::REGULAR:
				child = std::make_shared<RegularNode>(resp.id(), pull_node.descriptor());
				break;
			case managarm::fs::FileType::SYMLINK:
				child = std::make_shared<SymlinkNode>(pull_node.descriptor());
				break;
			default:
				throw std::runtime_error("extern_fs: Unexpected file type");
			}
			auto intern = _context->internalizeNode(resp.id(), child);
			auto link = std::make_shared<Entry>(shared_from_this(), intern);
			COFIBER_RETURN(_context->internalizeLink(this, name, link));
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
		COFIBER_RETURN(File::constructHandle(std::move(file)));
	}))

public:
	DirectoryNode(Context *context, helix::UniqueLane lane)
	: _context{context}, _lane{std::move(lane)} { }

private:
	Context *_context;
	helix::UniqueLane _lane;
};

std::shared_ptr<FsNode> Context::internalizeNode(int64_t id, std::shared_ptr<FsNode> node) {
	auto it = _activeNodes.find(id);
	if(it != _activeNodes.end()) {
		auto intern = it->second.lock();
		if(intern) {
			return intern;
		}else{
			it->second = node;
			return node;
		}
	}
	_activeNodes.insert({id, std::weak_ptr<FsNode>{node}});
	return node;
}

std::shared_ptr<FsLink> Context::internalizeLink(FsNode *parent, std::string name, std::shared_ptr<FsLink> link) {
	auto it = _activeLinks.find({parent, name});
	if(it != _activeLinks.end()) {
		auto intern = it->second.lock();
		if(intern) {
			return intern;
		}else{
			it->second = link;
			return link;
		}
	}
	_activeLinks.insert({{parent, name}, std::weak_ptr<FsLink>{link}});
	return link;
}

} // anonymous namespace

std::shared_ptr<FsLink> createRoot(helix::UniqueLane lane) {
	auto context = new Context{};
	auto node = std::make_shared<DirectoryNode>(context, std::move(lane));
	// FIXME: 2 is the ext2fs root inode.
	auto intern = context->internalizeNode(2, node);
	auto link = std::make_shared<Entry>(nullptr, intern);
	return context->internalizeLink(nullptr, std::string{}, link);
}

smarter::shared_ptr<File, FileHandle>
createFile(helix::UniqueLane lane, std::shared_ptr<FsLink> link) {
	return File::constructHandle(smarter::make_shared<OpenFile>(helix::UniqueLane{},
			std::move(lane), std::move(link)));
}

} // namespace extern_fs

