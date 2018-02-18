
#include <fcntl.h>
#include <unistd.h>
#include <set>

#include <protocols/fs/client.hpp>
#include <protocols/fs/server.hpp>
#include "common.hpp"
#include "tmp_fs.hpp"

// TODO: Remove dependency on those functions.
#include "extern_fs.hpp"
HelHandle __mlibc_getPassthrough(int fd);
HelHandle __raw_map(int fd);

namespace tmp_fs {

namespace {

struct Superblock;

struct Symlink : FsNode {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		std::cout << "\e[31mposix: Fix tmpfs Symlink::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(FileStats{});
	}))

	COFIBER_ROUTINE(expected<std::string>, readSymlink() override, ([=] {
		COFIBER_RETURN(_link);
	}))

public:
	Symlink(std::string link)
	: _link(std::move(link)) { }

private:
	std::string _link;
};

struct DeviceFile : FsNode {
private:
	VfsType getType() override {
		return _type;
	}
	
	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		std::cout << "\e[31mposix: Fix tmpfs DeviceFile::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(FileStats{});
	}))

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

struct MyLink : FsLink {
private:
	std::shared_ptr<FsNode> getOwner() override {
		return owner;
	}

	std::string getName() override {
		return name;
	}

	std::shared_ptr<FsNode> getTarget() override {
		return target;
	}

public:
	explicit MyLink(std::shared_ptr<FsNode> owner, std::string name, std::shared_ptr<FsNode> target)
	: owner(std::move(owner)), name(std::move(name)), target(std::move(target)) { }

	std::shared_ptr<FsNode> owner;
	std::string name;
	std::shared_ptr<FsNode> target;
};

struct Directory : FsNode, std::enable_shared_from_this<Directory> {
private:
	VfsType getType() override {
		return VfsType::directory;
	}

	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		std::cout << "\e[31mposix: Fix tmpfs Directory::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(FileStats{});
	}))

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>,
			getLink(std::string name) override, ([=] {
		auto it = _entries.find(name);
		if(it != _entries.end())
			COFIBER_RETURN(*it);
		COFIBER_RETURN(nullptr); // TODO: Return an error code.
	}))

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>, link(std::string name,
			std::shared_ptr<FsNode> target) override, ([=] {
		assert(_entries.find(name) == _entries.end());
		auto link = std::make_shared<MyLink>(shared_from_this(), std::move(name), std::move(target));
		_entries.insert(link);
		COFIBER_RETURN(link);
	}))

	FutureMaybe<std::shared_ptr<FsLink>> mkdir(std::string name) override;
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>,
			symlink(std::string name, std::string path), ([=] {
		assert(_entries.find(name) == _entries.end());
		auto node = std::make_shared<Symlink>(std::move(path));
		auto link = std::make_shared<MyLink>(shared_from_this(), std::move(name), std::move(node));
		_entries.insert(link);
		COFIBER_RETURN(link);
	}))
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>, mkdev(std::string name,
			VfsType type, DeviceId id), ([=] {
		assert(_entries.find(name) == _entries.end());
		auto node = std::make_shared<DeviceFile>(type, id);
		auto link = std::make_shared<MyLink>(shared_from_this(), std::move(name), std::move(node));
		_entries.insert(link);
		COFIBER_RETURN(link);
	}))

public:
	Directory(Superblock *superblock);

private:
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

// TODO: Remove this class in favor of MemoryNode.
struct InheritedNode : FsNode {
private:
	VfsType getType() override {
		return VfsType::regular;
	}

	FutureMaybe<FileStats> getStats() override {
		assert(!"Fix this");
	}

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<ProperFile>>,
			open(std::shared_ptr<FsLink> link) override, ([=] {
		auto fd = ::open(_path.c_str(), O_RDONLY);
		assert(fd != -1);

		helix::UniqueDescriptor passthrough(__mlibc_getPassthrough(fd));
		COFIBER_RETURN(extern_fs::createFile(std::move(passthrough), std::move(link)));
	}))

public:
	InheritedNode(std::string path)
	: _path{std::move(path)} { }

private:
	std::string _path;
};

struct MemoryFile : ProperFile {
private:
	// ------------------------------------------------------------------------
	// File protocol adapters.
	// ------------------------------------------------------------------------
private:
	static async::result<size_t> ptRead(std::shared_ptr<void> object,
			void *buffer, size_t length) {
		auto self = static_cast<MemoryFile *>(object.get());
		return self->readSome(buffer, length);
	}
	
	static async::result<void> ptWrite(std::shared_ptr<void> object,
			const void *buffer, size_t length) {
		auto self = static_cast<MemoryFile *>(object.get());
		return self->writeAll(buffer, length);
	}
	
	static constexpr auto fileOperations = protocols::fs::FileOperations{}
			.withRead(&ptRead)
			.withWrite(&ptWrite);

public:
	static void serve(std::shared_ptr<MemoryFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), file,
				&fileOperations);
	}

	MemoryFile()
	: ProperFile{nullptr} { }
	
	COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		assert(!"Fix MemoryFile::readSome");
	}))
	
	COFIBER_ROUTINE(FutureMaybe<void>, writeAll(const void *data, size_t length), ([=] {
		assert(!"Fix MemoryFile::writeAll");
	}))
	
	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
};

struct MemoryNode : FsNode {
private:
	VfsType getType() override {
		return VfsType::regular;
	}

	FutureMaybe<FileStats> getStats() override {
		assert(!"Fix this");
	}

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<ProperFile>>,
			open(std::shared_ptr<FsLink> link) override, ([=] {
		auto file = std::make_shared<MemoryFile>();
		MemoryFile::serve(file);
		COFIBER_RETURN(std::move(file));
	}))

public:
	MemoryNode() { }

private:
};

struct Superblock : FsSuperblock {
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsNode>>,
			createRegular() override, ([=] {
		auto node = std::make_shared<MemoryNode>();
		COFIBER_RETURN(std::move(node));
	}))
};

Directory::Directory(Superblock *superblock)
: FsNode{superblock} { }

COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>,
		Directory::mkdir(std::string name), ([=] {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<Directory>(static_cast<Superblock *>(superblock()));
	auto link = std::make_shared<MyLink>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	COFIBER_RETURN(link);
}))

// TODO: File system should not have global superblocks.
static Superblock globalSuperblock;

} // anonymous namespace

// Ironically, this function does not create a MemoryNode.
std::shared_ptr<FsNode> createMemoryNode(std::string path) {
	return std::make_shared<InheritedNode>(std::move(path));
}

std::shared_ptr<FsLink> createRoot() {
	auto node = std::make_shared<Directory>(&globalSuperblock);
	return std::make_shared<MyLink>(nullptr, std::string{}, std::move(node));
}

} // namespace tmp_fs

