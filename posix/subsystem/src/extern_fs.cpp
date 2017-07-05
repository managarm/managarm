
#include <unistd.h>
#include <fcntl.h>

#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "extern_fs.hpp"
#include "fs.pb.h"

HelHandle __mlibc_getPassthrough(int fd);
HelHandle __raw_map(int fd);

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

struct OpenFile : File {
private:
	static COFIBER_ROUTINE(FutureMaybe<off_t>, seek(std::shared_ptr<File> object,
			off_t offset, VfsSeek whence), ([=] {
		auto derived = std::static_pointer_cast<OpenFile>(object);
		assert(whence == VfsSeek::absolute);
		COFIBER_AWAIT derived->_file.seekAbsolute(offset);
		COFIBER_RETURN(offset);
	}))

	static COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(std::shared_ptr<File> object,
			void *data, size_t max_length), ([=] {
		auto derived = std::static_pointer_cast<OpenFile>(object);
		size_t length = COFIBER_AWAIT derived->_file.readSome(data, max_length);
		COFIBER_RETURN(length);
	}))

	static COFIBER_ROUTINE(FutureMaybe<helix::UniqueDescriptor>,
			accessMemory(std::shared_ptr<File> object), ([=] {
		auto derived = std::static_pointer_cast<OpenFile>(object);
		auto memory = COFIBER_AWAIT derived->_file.accessMemory();
		COFIBER_RETURN(std::move(memory));
	}))

	static helix::BorrowedDescriptor getPassthroughLane(std::shared_ptr<File> object) {
		auto derived = std::static_pointer_cast<OpenFile>(object);
		return derived->_file.getLane();
	}

	static const FileOperations operations;

public:
	OpenFile(helix::UniqueLane lane, std::shared_ptr<Node> node)
	: File{std::move(node), &operations}, _file{std::move(lane)} { }

private:
	protocols::fs::File _file;
};
	
const FileOperations OpenFile::operations{
	&OpenFile::seek,
	&OpenFile::readSome,
	&OpenFile::accessMemory,
	&OpenFile::getPassthroughLane
};

struct Regular : Node {
private:
	static FileStats getStats(std::shared_ptr<Node>) {
		assert(!"Fix this");
	}

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>,
			open(std::shared_ptr<Node> object), ([=] {
		auto self = std::static_pointer_cast<Regular>(object);

		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::PullDescriptor pull_passthrough;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_OPEN);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(self->_lane, helix::Dispatcher::global(),
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
		COFIBER_RETURN(std::make_shared<OpenFile>(pull_passthrough.descriptor(), self));
	}))

	static const NodeOperations operations;

public:
	Regular(helix::UniqueLane lane)
	: Node{&operations}, _lane{std::move(lane)} { }

private:
	helix::UniqueLane _lane;
};

const NodeOperations Regular::operations{
	&getRegularType,
	&Regular::getStats,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	&Regular::open,
	nullptr,
	nullptr
};

struct Entry : Link {
private:
	static std::shared_ptr<Node> getOwner(std::shared_ptr<Link> object) {
		auto derived = std::static_pointer_cast<Entry>(object);
		return derived->_owner;
	}

	static std::string getName(std::shared_ptr<Link> object) {
		(void)object;
		assert(!"No associated name");
	}

	static std::shared_ptr<Node> getTarget(std::shared_ptr<Link> object) {
		auto derived = std::static_pointer_cast<Entry>(object);
		auto shared = derived->_target.lock();
		assert(shared);
		return shared;
	}
	
	static const LinkOperations operations;

public:
	Entry(std::shared_ptr<Node> owner, std::weak_ptr<Node> target)
	: Link(&operations), _owner{std::move(owner)}, _target{std::move(target)} { }

private:
	std::shared_ptr<Node> _owner;
	std::weak_ptr<Node> _target;
};

const LinkOperations Entry::operations{
	&Entry::getOwner,
	&Entry::getName,
	&Entry::getTarget
};

struct Directory : Node {
private:
	static FileStats getStats(std::shared_ptr<Node>) {
		assert(!"Fix this");
	}

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			mkdir(std::shared_ptr<Node> object, std::string name), ([=] {
		(void)object;
		(void)name;
		assert(!"mkdir is not implemented for extern_fs");
	}))
	
	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			symlink(std::shared_ptr<Node> object, std::string name, std::string link), ([=] {
		(void)object;
		(void)name;
		(void)link;
		assert(!"symlink is not implemented for extern_fs");
	}))
	
	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>, mkdev(std::shared_ptr<Node> object,
			std::string name, VfsType type, DeviceId id), ([=] {
		(void)object;
		(void)name;
		(void)type;
		(void)id;
		assert(!"mkdev is not implemented for extern_fs");
	}))

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			getLink(std::shared_ptr<Node> object, std::string name), ([=] {
		auto self = std::static_pointer_cast<Directory>(object);
//		std::cout << "extern_fs: getLink() " << name << std::endl;

		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::PullDescriptor pull_node;

		managarm::fs::CntRequest req;
		req.set_req_type(managarm::fs::CntReqType::NODE_GET_LINK);
		req.set_path(name);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(self->_lane, helix::Dispatcher::global(),
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
				child = std::make_shared<Directory>(self->_context, pull_node.descriptor());
				break;
			case managarm::fs::FileType::REGULAR:
				child = std::make_shared<Regular>(pull_node.descriptor());
				break;
			default:
				throw std::runtime_error("extern_fs: Unexpected file type");
			}
			auto intern = self->_context->internalizeNode(resp.id(), child);
			auto link = std::make_shared<Entry>(self, intern);
			COFIBER_RETURN(self->_context->internalizeLink(self.get(), name, link));
		}else{
			COFIBER_RETURN(nullptr);
		}
	}))

	static const NodeOperations operations;

public:
	Directory(Context *context, helix::UniqueLane lane)
	: Node{&operations}, _context{context}, _lane{std::move(lane)} { }

private:
	Context *_context;
	helix::UniqueLane _lane;
};

const NodeOperations Directory::operations{
	&getDirectoryType,
	&Directory::getStats,
	&Directory::getLink,
	&Directory::mkdir,
	&Directory::symlink,
	&Directory::mkdev,
	nullptr,
	nullptr,
	nullptr
};

struct FakeRegular : Node {
private:
	static FileStats getStats(std::shared_ptr<Node>) {
		assert(!"Fix this");
	}

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>,
			open(std::shared_ptr<Node> object), ([=] {
		auto derived = std::static_pointer_cast<FakeRegular>(object);
		helix::UniqueDescriptor passthrough(__mlibc_getPassthrough(derived->_fd));
		COFIBER_RETURN(std::make_shared<OpenFile>(std::move(passthrough), derived));
	}))

	static const NodeOperations operations;

public:
	FakeRegular(int fd)
	: Node(&operations), _fd(fd) { }

private:
	int _fd;
};

const NodeOperations FakeRegular::operations{
	&getRegularType,
	&FakeRegular::getStats,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	&FakeRegular::open,
	nullptr,
	nullptr
};

struct FakeDirectory : Node {
private:
	static FileStats getStats(std::shared_ptr<Node>) {
		assert(!"Fix this");
	}

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			mkdir(std::shared_ptr<Node> object, std::string name), ([=] {
		(void)object;
		(void)name;
		assert(!"mkdir is not implemented for extern_fs");
	}))
	
	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			symlink(std::shared_ptr<Node> object, std::string name, std::string link), ([=] {
		(void)object;
		(void)name;
		(void)link;
		assert(!"symlink is not implemented for extern_fs");
	}))
	
	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>, mkdev(std::shared_ptr<Node> object,
			std::string name, VfsType type, DeviceId id), ([=] {
		(void)object;
		(void)name;
		(void)type;
		(void)id;
		assert(!"mkdev is not implemented for extern_fs");
	}))

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			getLink(std::shared_ptr<Node> object, std::string name), ([=] {
		(void)object;
		int fd = open(name.c_str(), O_RDONLY);
		auto node = std::make_shared<FakeRegular>(fd);
		// TODO: do not use createRootLink here!
		COFIBER_RETURN(createRootLink(std::move(node)));
	}))

	static const NodeOperations operations;

public:
	FakeDirectory()
	: Node(&operations) { }
};

const NodeOperations FakeDirectory::operations{
	&getDirectoryType,
	&FakeDirectory::getStats,
	&FakeDirectory::getLink,
	&FakeDirectory::mkdir,
	&FakeDirectory::symlink,
	&FakeDirectory::mkdev,
	nullptr,
	nullptr,
	nullptr
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

std::shared_ptr<Link> createRoot() {
	auto node = std::make_shared<FakeDirectory>();
	return createRootLink(std::move(node));
}

std::shared_ptr<Link> createRoot(helix::UniqueLane lane) {
	auto context = new Context{};
	auto node = std::make_shared<Directory>(context, std::move(lane));
	return createRootLink(std::move(node));
}

std::shared_ptr<File> createFile(helix::UniqueLane lane, std::shared_ptr<Node> node) {
	return std::make_shared<OpenFile>(std::move(lane), std::move(node));
}

} // namespace extern_fs

