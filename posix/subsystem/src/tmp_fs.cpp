
#include <fcntl.h>
#include <unistd.h>
#include <set>

#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "tmp_fs.hpp"

// TODO: Remove dependency on those functions.
#include "extern_fs.hpp"
HelHandle __mlibc_getPassthrough(int fd);
HelHandle __raw_map(int fd);

namespace tmp_fs {

namespace {

struct Symlink : Node {
private:
	static FileStats getStats(std::shared_ptr<Node>) {
		std::cout << "\e[31mposix: Fix tmpfs Symlink::getStats()\e[39m" << std::endl;
		return FileStats{};
	}

	static COFIBER_ROUTINE(FutureMaybe<std::string>,
			readSymlink(std::shared_ptr<Node> object), ([=] {
		auto derived = std::static_pointer_cast<Symlink>(object);
		COFIBER_RETURN(derived->_link);
	}))

	static const NodeOperations operations;

public:
	Symlink(std::string link)
	: Node(&operations), _link(std::move(link)) { }

private:
	std::string _link;
};

const NodeOperations Symlink::operations{
	&getSymlinkType,
	&Symlink::getStats,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	&Symlink::readSymlink,
	nullptr
};

struct DeviceFile : Node {
private:
	static VfsType getType(std::shared_ptr<Node> object) {
		auto derived = std::static_pointer_cast<DeviceFile>(object);
		return derived->_type;
	}
	
	static FileStats getStats(std::shared_ptr<Node>) {
		std::cout << "\e[31mposix: Fix tmpfs DeviceFile::getStats()\e[39m" << std::endl;
		return FileStats{};
	}

	static DeviceId readDevice(std::shared_ptr<Node> object) {
		auto derived = std::static_pointer_cast<DeviceFile>(object);
		return derived->_id;
	}

	static const NodeOperations operations;

public:
	DeviceFile(VfsType type, DeviceId id)
	: Node(&operations), _type(type), _id(id) {
		assert(type == VfsType::charDevice || type == VfsType::blockDevice);
	}

private:
	VfsType _type;
	DeviceId _id;
};

const NodeOperations DeviceFile::operations{
	&DeviceFile::getType,
	&DeviceFile::getStats,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	&DeviceFile::readDevice
};

struct Directory : Node {
private:
	static FileStats getStats(std::shared_ptr<Node>) {
		std::cout << "\e[31mposix: Fix tmpfs Directory::getStats()\e[39m" << std::endl;
		return FileStats{};
	}

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			getLink(std::shared_ptr<Node> object, std::string name), ([=] {
		auto derived = std::static_pointer_cast<Directory>(object);
		auto it = derived->_entries.find(name);
		if(it != derived->_entries.end())
			COFIBER_RETURN(*it);
		COFIBER_RETURN(nullptr); // TODO: Return an error code.
	}))

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>, link(std::shared_ptr<Node> object,
			std::string name, std::shared_ptr<Node> target), ([=] {
		auto derived = std::static_pointer_cast<Directory>(object);
		assert(derived->_entries.find(name) == derived->_entries.end());
		auto link = std::make_shared<MyLink>(object, std::move(name), std::move(target));
		derived->_entries.insert(link);
		COFIBER_RETURN(link);
	}))

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			mkdir(std::shared_ptr<Node> object, std::string name), ([=] {
		auto derived = std::static_pointer_cast<Directory>(object);
		assert(derived->_entries.find(name) == derived->_entries.end());
		auto node = std::make_shared<Directory>();
		auto link = std::make_shared<MyLink>(object, std::move(name), std::move(node));
		derived->_entries.insert(link);
		COFIBER_RETURN(link);
	}))
	
	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			symlink(std::shared_ptr<Node> object, std::string name, std::string path), ([=] {
		auto derived = std::static_pointer_cast<Directory>(object);
		assert(derived->_entries.find(name) == derived->_entries.end());
		auto node = std::make_shared<Symlink>(std::move(path));
		auto link = std::make_shared<MyLink>(object, std::move(name), std::move(node));
		derived->_entries.insert(link);
		COFIBER_RETURN(link);
	}))
	
	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>, mkdev(std::shared_ptr<Node> object,
			std::string name, VfsType type, DeviceId id), ([=] {
		auto derived = std::static_pointer_cast<Directory>(object);
		assert(derived->_entries.find(name) == derived->_entries.end());
		auto node = std::make_shared<DeviceFile>(type, id);
		auto link = std::make_shared<MyLink>(object, std::move(name), std::move(node));
		derived->_entries.insert(link);
		COFIBER_RETURN(link);
	}))

	static const NodeOperations operations;

public:
	Directory()
	: Node(&operations) { }

private:
	struct MyLink : Link {
	private:
		static std::shared_ptr<Node> getOwner(std::shared_ptr<Link> object) {
			auto derived = std::static_pointer_cast<MyLink>(object);
			return derived->owner;
		}

		static std::string getName(std::shared_ptr<Link> object) {
			auto derived = std::static_pointer_cast<MyLink>(object);
			return derived->name;
		}

		static std::shared_ptr<Node> getTarget(std::shared_ptr<Link> object) {
			auto derived = std::static_pointer_cast<MyLink>(object);
			return derived->target;
		}

		static const LinkOperations operations;

	public:
		explicit MyLink(std::shared_ptr<Node> owner, std::string name, std::shared_ptr<Node> target)
		: Link(&operations), owner(std::move(owner)),
				name(std::move(name)), target(std::move(target)) { }

		std::shared_ptr<Node> owner;
		std::string name;
		std::shared_ptr<Node> target;
	};

	struct Compare {
		struct is_transparent { };

		bool operator() (const std::shared_ptr<MyLink> &link, const std::string &name) const {
			return link->name < name;
		}
		bool operator() (const std::string &name, const std::shared_ptr<MyLink> &link) const {
			return name < link->name;
		}

		bool operator() (const std::shared_ptr<MyLink> &a, const std::shared_ptr<MyLink> &b) const {
			return a->name < b->name;
		}
	};

	std::set<std::shared_ptr<MyLink>, Compare> _entries;
};

const NodeOperations Directory::operations{
	&getDirectoryType,
	&Directory::getStats,
	&Directory::getLink,
	&Directory::link,
	&Directory::mkdir,
	&Directory::symlink,
	&Directory::mkdev,
	nullptr,
	nullptr,
	nullptr
};

const LinkOperations Directory::MyLink::operations{
	&MyLink::getOwner,
	&MyLink::getName,
	&MyLink::getTarget
};

struct MemoryNode : Node {
private:
	static FileStats getStats(std::shared_ptr<Node>) {
		assert(!"Fix this");
	}

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>,
			open(std::shared_ptr<Node> object), ([=] {
		auto self = std::static_pointer_cast<MemoryNode>(object);

		auto fd = ::open(self->_path.c_str(), O_RDONLY);
		assert(fd != -1);

		helix::UniqueDescriptor passthrough(__mlibc_getPassthrough(fd));
		COFIBER_RETURN(extern_fs::createFile(std::move(passthrough), self));
	}))

	static const NodeOperations operations;

public:
	MemoryNode(std::string path)
	: Node(&operations), _path{std::move(path)} { }

private:
	std::string _path;
};

const NodeOperations MemoryNode::operations{
	&getRegularType,
	&MemoryNode::getStats,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	&MemoryNode::open,
	nullptr,
	nullptr
};

} // anonymous namespace

std::shared_ptr<Node> createMemoryNode(std::string path) {
	return std::make_shared<MemoryNode>(std::move(path));
}

std::shared_ptr<Link> createRoot() {
	auto node = std::make_shared<Directory>();
	return createRootLink(std::move(node));
}

} // namespace tmp_fs

