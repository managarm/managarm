
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
	VfsType getType() override {
		return VfsType::symlink;
	}

	FileStats getStats() override {
		std::cout << "\e[31mposix: Fix tmpfs Symlink::getStats()\e[39m" << std::endl;
		return FileStats{};
	}

	COFIBER_ROUTINE(FutureMaybe<std::string>, readSymlink() override, ([=] {
		COFIBER_RETURN(_link);
	}))

public:
	Symlink(std::string link)
	: _link(std::move(link)) { }

private:
	std::string _link;
};

struct DeviceFile : Node {
private:
	VfsType getType() override {
		return _type;
	}
	
	FileStats getStats() override {
		std::cout << "\e[31mposix: Fix tmpfs DeviceFile::getStats()\e[39m" << std::endl;
		return FileStats{};
	}

	DeviceId readDevice() override {
		return _id;
	}

public:
	DeviceFile(VfsType type, DeviceId id)
	: _type(type), _id(id) {
		assert(type == VfsType::charDevice || type == VfsType::blockDevice);
	}

private:
	VfsType _type;
	DeviceId _id;
};

struct Directory : Node, std::enable_shared_from_this<Directory> {
private:
	VfsType getType() override {
		return VfsType::directory;
	}

	FileStats getStats() override {
		std::cout << "\e[31mposix: Fix tmpfs Directory::getStats()\e[39m" << std::endl;
		return FileStats{};
	}

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			getLink(std::string name) override, ([=] {
		auto it = _entries.find(name);
		if(it != _entries.end())
			COFIBER_RETURN(*it);
		COFIBER_RETURN(nullptr); // TODO: Return an error code.
	}))

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>, link(std::string name,
			std::shared_ptr<Node> target) override, ([=] {
		assert(_entries.find(name) == _entries.end());
		auto link = std::make_shared<MyLink>(shared_from_this(), std::move(name), std::move(target));
		_entries.insert(link);
		COFIBER_RETURN(link);
	}))

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			mkdir(std::string name) override, ([=] {
		assert(_entries.find(name) == _entries.end());
		auto node = std::make_shared<Directory>();
		auto link = std::make_shared<MyLink>(shared_from_this(), std::move(name), std::move(node));
		_entries.insert(link);
		COFIBER_RETURN(link);
	}))
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			symlink(std::string name, std::string path), ([=] {
		assert(_entries.find(name) == _entries.end());
		auto node = std::make_shared<Symlink>(std::move(path));
		auto link = std::make_shared<MyLink>(shared_from_this(), std::move(name), std::move(node));
		_entries.insert(link);
		COFIBER_RETURN(link);
	}))
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>, mkdev(std::string name,
			VfsType type, DeviceId id), ([=] {
		assert(_entries.find(name) == _entries.end());
		auto node = std::make_shared<DeviceFile>(type, id);
		auto link = std::make_shared<MyLink>(shared_from_this(), std::move(name), std::move(node));
		_entries.insert(link);
		COFIBER_RETURN(link);
	}))

public:
	Directory() = default;

private:
	struct MyLink : Link {
	private:
		std::shared_ptr<Node> getOwner() override {
			return owner;
		}

		std::string getName() override {
			return name;
		}

		std::shared_ptr<Node> getTarget() override {
			return target;
		}

	public:
		explicit MyLink(std::shared_ptr<Node> owner, std::string name, std::shared_ptr<Node> target)
		: owner(std::move(owner)), name(std::move(name)), target(std::move(target)) { }

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

struct MemoryNode : Node, std::enable_shared_from_this<MemoryNode> {
private:
	VfsType getType() override {
		return VfsType::regular;
	}

	FileStats getStats() override {
		assert(!"Fix this");
	}

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>, open() override, ([=] {
		auto fd = ::open(_path.c_str(), O_RDONLY);
		assert(fd != -1);

		helix::UniqueDescriptor passthrough(__mlibc_getPassthrough(fd));
		COFIBER_RETURN(extern_fs::createFile(std::move(passthrough), shared_from_this()));
	}))

public:
	MemoryNode(std::string path)
	: _path{std::move(path)} { }

private:
	std::string _path;
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

