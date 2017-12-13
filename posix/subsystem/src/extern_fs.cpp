
#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "extern_fs.hpp"
#include "fs.pb.h"

namespace extern_fs {

namespace {

struct Context {

	std::shared_ptr<Node> internalizeNode(int64_t id, std::shared_ptr<Node> node);
	std::shared_ptr<Link> internalizeLink(Node *parent, std::string name,
			std::shared_ptr<Link> link);

private:
	std::map<int64_t, std::weak_ptr<Node>> _activeNodes;
	std::map<std::pair<Node *, std::string>, std::weak_ptr<Link>> _activeLinks;
};

struct OpenFile : ProperFile {
private:
	COFIBER_ROUTINE(FutureMaybe<off_t>, seek(off_t offset, VfsSeek whence) override, ([=] {
		assert(whence == VfsSeek::absolute);
		COFIBER_AWAIT _file.seekAbsolute(offset);
		COFIBER_RETURN(offset);
	}))

	COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		size_t length = COFIBER_AWAIT _file.readSome(data, max_length);
		COFIBER_RETURN(length);
	}))

	COFIBER_ROUTINE(FutureMaybe<helix::UniqueDescriptor>, accessMemory() override, ([=] {
		auto memory = COFIBER_AWAIT _file.accessMemory();
		COFIBER_RETURN(std::move(memory));
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _file.getLane();
	}

public:
	OpenFile(helix::UniqueLane lane, std::shared_ptr<Link> link)
	: ProperFile{std::move(link)}, _file{std::move(lane)} { }

private:
	protocols::fs::File _file;
};

struct Regular : Node {
private:
	VfsType getType() override {
		return VfsType::regular;
	}

	FileStats getStats() override {
		assert(!"Fix this");
	}

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<ProperFile>>,
			open(std::shared_ptr<Link> link) override, ([=] {
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::PullDescriptor pull_passthrough;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_OPEN);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&pull_passthrough));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(pull_passthrough.error());

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::fs::Errors::SUCCESS);
		COFIBER_RETURN(std::make_shared<OpenFile>(pull_passthrough.descriptor(), std::move(link)));
	}))

public:
	Regular(helix::UniqueLane lane)
	: _lane{std::move(lane)} { }

private:
	helix::UniqueLane _lane;
};

struct Symlink : Node {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	FileStats getStats() override {
		throw std::runtime_error("extern_fs: Fix Symlink::getStats()");
	}

	COFIBER_ROUTINE(FutureMaybe<std::string>, readSymlink() override, ([=] {
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

		managarm::fs::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::fs::Errors::SUCCESS);
		
		COFIBER_RETURN((std::string{static_cast<char *>(recv_target.data()), recv_target.length()}));
	}))

public:
	Symlink(helix::UniqueLane lane)
	: _lane{std::move(lane)} { }

private:
	helix::UniqueLane _lane;
};

struct Entry : Link {
private:
	std::shared_ptr<Node> getOwner() override {
		return _owner;
	}

	std::string getName() override {
		assert(!"No associated name");
	}

	std::shared_ptr<Node> getTarget() override {
		return _target;
	}

public:
	Entry(std::shared_ptr<Node> owner, std::shared_ptr<Node> target)
	: _owner{std::move(owner)}, _target{std::move(target)} { }

private:
	std::shared_ptr<Node> _owner;
	std::shared_ptr<Node> _target;
};

struct Directory : Node, std::enable_shared_from_this<Directory> {
private:
	VfsType getType() override {
		return VfsType::directory;
	}

	FileStats getStats() override {
		assert(!"Fix this");
	}

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			mkdir(std::string name) override, ([=] {
		(void)name;
		assert(!"mkdir is not implemented for extern_fs");
	}))
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			symlink(std::string name, std::string link) override, ([=] {
		(void)name;
		(void)link;
		assert(!"symlink is not implemented for extern_fs");
	}))
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>, mkdev(std::string name,
			VfsType type, DeviceId id) override, ([=] {
		(void)name;
		(void)type;
		(void)id;
		assert(!"mkdev is not implemented for extern_fs");
	}))

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
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

			std::shared_ptr<Node> child;
			switch(resp.file_type()) {
			case managarm::fs::FileType::DIRECTORY:
				child = std::make_shared<Directory>(_context, pull_node.descriptor());
				break;
			case managarm::fs::FileType::REGULAR:
				child = std::make_shared<Regular>(pull_node.descriptor());
				break;
			case managarm::fs::FileType::SYMLINK:
				child = std::make_shared<Symlink>(pull_node.descriptor());
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

public:
	Directory(Context *context, helix::UniqueLane lane)
	: _context{context}, _lane{std::move(lane)} { }

private:
	Context *_context;
	helix::UniqueLane _lane;
};

std::shared_ptr<Node> Context::internalizeNode(int64_t id, std::shared_ptr<Node> node) {
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
	_activeNodes.insert({id, std::weak_ptr<Node>{node}});
	return node;
}

std::shared_ptr<Link> Context::internalizeLink(Node *parent, std::string name, std::shared_ptr<Link> link) {
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
	_activeLinks.insert({{parent, name}, std::weak_ptr<Link>{link}});
	return link;
}

} // anonymous namespace

std::shared_ptr<Link> createRoot(helix::UniqueLane lane) {
	auto context = new Context{};
	auto node = std::make_shared<Directory>(context, std::move(lane));
	// FIXME: 2 is the ext2fs root inode.
	auto intern = context->internalizeNode(2, node);
	auto link = std::make_shared<Entry>(nullptr, intern);
	return context->internalizeLink(nullptr, std::string{}, link);
}

std::shared_ptr<ProperFile> createFile(helix::UniqueLane lane, std::shared_ptr<Link> link) {
	return std::make_shared<OpenFile>(std::move(lane), std::move(link));
}

} // namespace extern_fs

