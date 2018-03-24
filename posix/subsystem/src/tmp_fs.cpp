
#include <fcntl.h>
#include <unistd.h>
#include <set>

#include <protocols/fs/client.hpp>
#include <protocols/fs/server.hpp>
#include "common.hpp"
#include "device.hpp"
#include "tmp_fs.hpp"

// TODO: Remove dependency on those functions.
#include "extern_fs.hpp"
HelHandle __mlibc_getPassthrough(int fd);
HelHandle __raw_map(int fd);

namespace tmp_fs {

namespace {

struct Superblock;

struct SymlinkNode : FsNode {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		std::cout << "\e[31mposix: Fix tmpfs SymlinkNode::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(FileStats{});
	}))

	COFIBER_ROUTINE(expected<std::string>, readSymlink(FsLink *) override, ([=] {
		COFIBER_RETURN(_link);
	}))

public:
	SymlinkNode(std::string link)
	: _link(std::move(link)) { }

private:
	std::string _link;
};

struct DeviceNode : FsNode {
private:
	VfsType getType() override {
		return _type;
	}
	
	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		std::cout << "\e[31mposix: Fix tmpfs DeviceNode::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(FileStats{});
	}))

	DeviceId readDevice() override {
		return _id;
	}
	
	FutureMaybe<SharedFilePtr> open(std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		return openDevice(_type, _id, std::move(link), semantic_flags);
	}

public:
	DeviceNode(VfsType type, DeviceId id)
	: _type(type), _id(id) {
		assert(type == VfsType::charDevice || type == VfsType::blockDevice);
	}

private:
	VfsType _type;
	DeviceId _id;
};

struct SocketNode : FsNode {
private:
	VfsType getType() override {
		return VfsType::socket;
	}
	
	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		std::cout << "\e[31mposix: Fix tmpfs SocketNode::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(FileStats{});
	}))
};

struct Link : FsLink {
public:
	explicit Link(std::shared_ptr<FsNode> owner, std::string name, std::shared_ptr<FsNode> target)
	: owner(std::move(owner)), name(std::move(name)), target(std::move(target)) { }
	
	std::shared_ptr<FsNode> getOwner() override {
		return owner;
	}

	std::string getName() override {
		return name;
	}

	std::shared_ptr<FsNode> getTarget() override {
		return target;
	}

private:
	std::shared_ptr<FsNode> owner;
	std::string name;
	std::shared_ptr<FsNode> target;
};

struct LinkCompare {
	struct is_transparent { };

	bool operator() (const std::shared_ptr<Link> &link, const std::string &name) const {
		return link->getName() < name;
	}
	bool operator() (const std::string &name, const std::shared_ptr<Link> &link) const {
		return name < link->getName();
	}

	bool operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const {
		return a->getName() < b->getName();
	}
};

struct DirectoryNode;

struct DirectoryFile : File {
public:
	static void serve(smarter::shared_ptr<DirectoryFile> file);

	explicit DirectoryFile(std::shared_ptr<FsLink> link);

	FutureMaybe<ReadEntriesResult> readEntries() override;
	helix::BorrowedDescriptor getPassthroughLane() override;

private:
	// TODO: Remove this and extract it from the associatedLink().
	DirectoryNode *_node;

	helix::UniqueLane _passthrough;
	std::set<std::shared_ptr<Link>, LinkCompare>::iterator _iter;
};

struct DirectoryNode : FsNode, std::enable_shared_from_this<DirectoryNode> {
	friend struct DirectoryFile;
private:
	VfsType getType() override {
		return VfsType::directory;
	}

	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		std::cout << "\e[31mposix: Fix tmpfs DirectoryNode::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(FileStats{});
	}))

	COFIBER_ROUTINE(FutureMaybe<SharedFilePtr>,
	open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags), ([=] {
		assert(!semantic_flags);

		auto file = smarter::make_shared<DirectoryFile>(std::move(link));
		DirectoryFile::serve(file);
		COFIBER_RETURN(File::constructHandle(std::move(file)));
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
		auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(target));
		_entries.insert(link);
		COFIBER_RETURN(link);
	}))

	FutureMaybe<std::shared_ptr<FsLink>> mkdir(std::string name) override;
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>,
			symlink(std::string name, std::string path), ([=] {
		assert(_entries.find(name) == _entries.end());
		auto node = std::make_shared<SymlinkNode>(std::move(path));
		auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
		_entries.insert(link);
		COFIBER_RETURN(link);
	}))
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>, mkdev(std::string name,
			VfsType type, DeviceId id), ([=] {
		assert(_entries.find(name) == _entries.end());
		auto node = std::make_shared<DeviceNode>(type, id);
		auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
		_entries.insert(link);
		COFIBER_RETURN(link);
	}))
	
	COFIBER_ROUTINE(FutureMaybe<void>, unlink(std::string name) override, ([=] {
		auto it = _entries.find(name);
		assert(it != _entries.end());
		_entries.erase(it);
		COFIBER_RETURN();
	}))

public:
	DirectoryNode(Superblock *superblock);

private:
	std::set<std::shared_ptr<Link>, LinkCompare> _entries;
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

	COFIBER_ROUTINE(FutureMaybe<SharedFilePtr>,
	open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override, ([=] {
		assert(!semantic_flags);
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

struct MemoryFile : File {
public:
	static void serve(smarter::shared_ptr<MemoryFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), file,
				&fileOperations);
	}

	MemoryFile(std::shared_ptr<FsLink> link)
	: File{StructName::get("tmpfs.regular"), std::move(link)} { }
	
	COFIBER_ROUTINE(expected<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		assert(!"Fix MemoryFile::readSome");
	}))
	
	COFIBER_ROUTINE(FutureMaybe<void>, writeAll(const void *data, size_t length) override, ([=] {
		assert(!"Fix MemoryFile::writeAll");
	}))
	
	FutureMaybe<void> truncate(size_t size) override;
	
	FutureMaybe<void> allocate(int64_t offset, size_t size) override;

	FutureMaybe<helix::UniqueDescriptor> accessMemory(off_t);
	
	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
};

struct MemoryNode : FsNode {
	friend struct MemoryFile;

private:
	VfsType getType() override {
		return VfsType::regular;
	}

	FutureMaybe<FileStats> getStats() override {
		assert(!"Fix this");
	}

	COFIBER_ROUTINE(FutureMaybe<SharedFilePtr>,
	open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override, ([=] {
		assert(!semantic_flags);
		auto file = smarter::make_shared<MemoryFile>(std::move(link));
		MemoryFile::serve(file);
		COFIBER_RETURN(File::constructHandle(std::move(file)));
	}))

public:
	MemoryNode()
	: _areaSize{0}, _fileSize{0} { }

	helix::UniqueDescriptor _memory;
	size_t _areaSize;
	size_t _fileSize;
};

COFIBER_ROUTINE(async::result<void>, MemoryFile::truncate(size_t size), ([=] {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	node->_fileSize = size;

	size_t aligned_size = (size + 0xFFF) & ~size_t(0xFFF);
	if(!node->_memory) {
		HelHandle handle;
		HEL_CHECK(helAllocateMemory(aligned_size, 0, &handle));
		node->_memory = helix::UniqueDescriptor{handle};
	}else if(size > node->_areaSize) {
		HEL_CHECK(helResizeMemory(node->_memory.getHandle(), aligned_size));
	}
	node->_areaSize = aligned_size;

	COFIBER_RETURN();
}))

COFIBER_ROUTINE(async::result<void>, MemoryFile::allocate(int64_t offset, size_t size), ([=] {
	assert(!offset);

	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	if(offset + size < node->_fileSize)
		COFIBER_RETURN();

	node->_fileSize = offset + size;

	size_t aligned_size = (size + 0xFFF) & ~size_t(0xFFF);
	if(!node->_memory) {
		HelHandle handle;
		HEL_CHECK(helAllocateMemory(aligned_size, 0, &handle));
		node->_memory = helix::UniqueDescriptor{handle};
	}else if(offset + size > node->_areaSize) {
		HEL_CHECK(helResizeMemory(node->_memory.getHandle(), aligned_size));
	}
	node->_areaSize = aligned_size;

	COFIBER_RETURN();
}))	

COFIBER_ROUTINE(FutureMaybe<helix::UniqueDescriptor>, MemoryFile::accessMemory(off_t offset),
		([=] {
	assert(!offset);
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());
	COFIBER_RETURN(node->_memory.dup());
}))

// ----------------------------------------------------------------------------
// DirectoryFile implementation.
// ----------------------------------------------------------------------------

void DirectoryFile::serve(smarter::shared_ptr<DirectoryFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	protocols::fs::servePassthrough(std::move(lane), smarter::shared_ptr<File>{file},
			&File::fileOperations);
}

DirectoryFile::DirectoryFile(std::shared_ptr<FsLink> link)
: File{StructName::get("sysfs.dir"), std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())},
		_iter{_node->_entries.begin()} { }

// TODO: This iteration mechanism only works as long as _iter is not concurrently deleted.
COFIBER_ROUTINE(async::result<ReadEntriesResult>,
DirectoryFile::readEntries(), ([=] {
	if(_iter != _node->_entries.end()) {
		auto name = (*_iter)->getName();
		_iter++;
		COFIBER_RETURN(name);
	}else{
		COFIBER_RETURN(std::nullopt);
	}
}))

helix::BorrowedDescriptor DirectoryFile::getPassthroughLane() {
	return _passthrough;
}

struct Superblock : FsSuperblock {
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsNode>>, createRegular() override, ([=] {
		auto node = std::make_shared<MemoryNode>();
		COFIBER_RETURN(std::move(node));
	}))
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsNode>>, createSocket() override, ([=] {
		auto node = std::make_shared<SocketNode>();
		COFIBER_RETURN(std::move(node));
	}))

	COFIBER_ROUTINE(async::result<std::shared_ptr<FsLink>>, rename(FsLink *source,
			FsNode *directory, std::string name), ([=] {
		std::cout << "\e[31mposix: Fix tmpfs Superblock::rename()\e[39m" << std::endl;
	}))
};

DirectoryNode::DirectoryNode(Superblock *superblock)
: FsNode{superblock} { }

COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>,
		DirectoryNode::mkdir(std::string name), ([=] {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<DirectoryNode>(static_cast<Superblock *>(superblock()));
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
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
	auto node = std::make_shared<DirectoryNode>(&globalSuperblock);
	return std::make_shared<Link>(nullptr, std::string{}, std::move(node));
}

} // namespace tmp_fs

