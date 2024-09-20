#include <fcntl.h>
#include <unistd.h>
#include <set>

#include <helix/memory.hpp>
#include <helix/passthrough-fd.hpp>
#include <protocols/fs/client.hpp>
#include <protocols/fs/server.hpp>
#include "common.hpp"
#include "device.hpp"
#include "tmp_fs.hpp"
#include "fifo.hpp"
#include "process.hpp"
#include <sys/stat.h>

#include <bitset>

// TODO: Remove dependency on those functions.
#include "extern_fs.hpp"
HelHandle __raw_map(int fd);

namespace tmp_fs {

namespace {

struct Superblock;

struct Node : FsNode {
	Node(Superblock *superblock, FsNode::DefaultOps default_ops = 0);

protected:
	~Node() = default;

	void initializeMode(mode_t mode) {
		_mode = mode;
	}

public:
	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "\e[31mposix: Fix tmpfs getStats()\e[39m" << std::endl;
		FileStats stats{};
		stats.inodeNumber = _inodeNumber;
		stats.fileSize = 0;
		stats.numLinks = _numLinks;
		stats.mode = _mode;
		stats.uid = _uid;
		stats.gid = _gid;
		stats.atimeSecs = _atime.tv_sec;
		stats.atimeNanos = _atime.tv_nsec;
		stats.mtimeSecs = _mtime.tv_sec;
		stats.mtimeNanos = _mtime.tv_nsec;
		stats.ctimeSecs = _ctime.tv_sec;
		stats.ctimeNanos = _ctime.tv_nsec;
		co_return stats;
	}

	int64_t inodeNumber() {
		return _inodeNumber;
	}

	int numLinks() {
		return _numLinks;
	}

	mode_t mode() {
		return _mode;
	}

	uid_t uid() {
		return _uid;
	}

	gid_t gid() {
		return _gid;
	}

	timespec atime() {
		return _atime;
	}

	timespec mtime() {
		return _mtime;
	}

	timespec ctime() {
		return _ctime;
	}

	async::result<Error> chmod(int mode) override {
		_mode = (_mode & 0xFFFFF000) | mode;
		co_return Error::success;
	}

	async::result<Error> utimensat(uint64_t atime_sec, uint64_t atime_nsec, uint64_t mtime_sec, uint64_t mtime_nsec) override {
		if(atime_sec != UTIME_NOW || atime_nsec != UTIME_NOW || mtime_sec != UTIME_NOW || mtime_nsec != UTIME_NOW) {
			std::cout << "\e[31m" "tmp_fs: utimensat() only supports setting atime and mtime to current time" "\e[39m" << std::endl;
			co_return Error::success;
		}
		struct timespec time;
		// TODO: Move to CLOCK_REALTIME when supported
		clock_gettime(CLOCK_MONOTONIC, &time);
		_atime.tv_sec = time.tv_sec;
		_atime.tv_nsec = time.tv_nsec;
		_mtime.tv_sec = time.tv_sec;
		_mtime.tv_nsec = time.tv_nsec;
		co_return Error::success;
	}

private:
	int64_t _inodeNumber;
	int _numLinks = 0;
	mode_t _mode = 0;
	uid_t _uid = 0;
	gid_t _gid = 0;
	timespec _atime = {0, 0};
	timespec _mtime = {0, 0};
	timespec _ctime = {0, 0};
};

struct SymlinkNode final : Node {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	expected<std::string> readSymlink(FsLink *, Process *) override {
		co_return _link;
	}

public:
	SymlinkNode(Superblock *superblock, std::string link);

private:
	std::string _link;
};

struct DeviceNode final : Node {
private:
	VfsType getType() override {
		return _type;
	}

	DeviceId readDevice() override {
		return _id;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> open(std::shared_ptr<MountView> mount,
			std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override {
		return openDevice(_type, _id, std::move(mount), std::move(link), semantic_flags);
	}

public:
	DeviceNode(Superblock *superblock, VfsType type, DeviceId id);

private:
	VfsType _type;
	DeviceId _id;
};

struct SocketNode final : Node {
	SocketNode(Superblock *superblock);

	VfsType getType() override {
		return VfsType::socket;
	}
};

struct FifoNode final : Node {
private:
	VfsType getType() override {
		return VfsType::fifo;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> open(std::shared_ptr<MountView> mount,
			std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override {
		co_return co_await fifo::openNamedChannel(mount, link, this, semantic_flags);
	}

public:
	FifoNode(Superblock *superblock, mode_t mode)
	:Node{superblock} {
		fifo::createNamedChannel(this);
		initializeMode(mode);
	}

	~FifoNode() {
		fifo::unlinkNamedChannel(this);
	}
};

struct Link final : FsLink {
public:
	explicit Link(std::shared_ptr<FsNode> target)
	: _target(std::move(target)) { }

	explicit Link(std::shared_ptr<FsNode> owner, std::string name, std::shared_ptr<FsNode> target)
	: _owner(std::move(owner)), _name(std::move(name)), _target(std::move(target)) {
		assert(_owner);
		assert(!_name.empty());
	}

	std::shared_ptr<FsNode> getOwner() override {
		return _owner;
	}

	std::string getName() override {
		// The root link does not have a name.
		assert(_owner);
		return _name;
	}

	std::shared_ptr<FsNode> getTarget() override {
		return _target;
	}

private:
	std::shared_ptr<FsNode> _owner;
	std::string _name;
	std::shared_ptr<FsNode> _target;
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

struct DirectoryFile final : File {
public:
	static void serve(smarter::shared_ptr<DirectoryFile> file);

	explicit DirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link);

	void handleClose() override;

	FutureMaybe<ReadEntriesResult> readEntries() override;
	helix::BorrowedDescriptor getPassthroughLane() override;

private:
	// TODO: Remove this and extract it from the associatedLink().
	DirectoryNode *_node;

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	std::set<std::shared_ptr<Link>, LinkCompare>::iterator _iter;
};

struct DirectoryNode final : Node, std::enable_shared_from_this<DirectoryNode> {
	friend struct Superblock;
	friend struct DirectoryFile;

	static std::shared_ptr<Link> createRootDirectory(Superblock *superblock);

private:
	VfsType getType() override {
		return VfsType::directory;
	}

	std::shared_ptr<FsLink> treeLink() override {
		return _treeLink;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		auto file = smarter::make_shared<DirectoryFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		DirectoryFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}


	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> getLink(std::string name) override {
		auto it = _entries.find(name);
		if(it != _entries.end())
			co_return *it;
		co_return nullptr; // TODO: Return an error code.
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> link(std::string name,
			std::shared_ptr<FsNode> target) override {
		if(!(_entries.find(name) == _entries.end()))
			co_return Error::alreadyExists;
		auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(target));
		_entries.insert(link);
		co_return link;
	}

	async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	mkdir(std::string name) override;

	async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	symlink(std::string name, std::string path) override;

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> mkdev(std::string name,
			VfsType type, DeviceId id) override;

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> mkfifo(std::string name, mode_t mode) override;

	async::result<frg::expected<Error>> unlink(std::string name) override {
		auto it = _entries.find(name);
		if(it == _entries.end())
			co_return Error::noSuchFile;

		auto target = it->get()->getTarget();
		if(target->getType() == VfsType::directory) {
			auto dir_target = reinterpret_cast<DirectoryNode *>(target.get());
			
			if(dir_target->_entries.size()) {
				co_return Error::directoryNotEmpty;
			}
		}

		_entries.erase(it);

		notifyObservers(FsObserver::deleteEvent, name, 0);
		co_return {};
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> mksocket(std::string name) override;

	async::result<frg::expected<Error>> rmdir(std::string name) override {
		auto result = co_await unlink(name);
		if(!result) {
			assert(result.error() == Error::noSuchFile
				|| result.error() == Error::directoryNotEmpty);
			co_return result.error();
		}
		co_return {};
	}

public:
	DirectoryNode(Superblock *superblock);

private:
	// TODO: This creates a circular reference -- fix this.
	std::shared_ptr<Link> _treeLink;
	std::set<std::shared_ptr<Link>, LinkCompare> _entries;
};

// TODO: Remove this class in favor of MemoryNode.
struct InheritedNode final : Node {
private:
	VfsType getType() override {
		return VfsType::regular;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: tmp_fs open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}
		auto fd = ::open(_path.c_str(), O_RDONLY);
		assert(fd != -1);

		helix::UniqueDescriptor passthrough(helix::handleForFd(fd));
		co_return extern_fs::createFile(std::move(passthrough), std::move(mount), std::move(link));
	}

public:
	InheritedNode(Superblock *superblock, std::string path);

private:
	std::string _path;
};

struct MemoryFile final : File {
public:
	static void serve(smarter::shared_ptr<MemoryFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	MemoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: File{StructName::get("tmpfs.regular"), std::move(mount), std::move(link)}, _offset{0} { }

	void handleClose() override;

	async::result<frg::expected<Error, off_t>> seek(off_t delta, VfsSeek whence) override;

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *buffer, size_t max_length) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *buffer, size_t length) override;

	async::result<frg::expected<Error, size_t>>
	pread(Process *, int64_t offset, void *buffer, size_t length) override;

	async::result<frg::expected<Error, size_t>>
	pwrite(Process *, int64_t offset, const void *buffer, size_t length) override;

	async::result<frg::expected<protocols::fs::Error>> truncate(size_t size) override;

	async::result<frg::expected<protocols::fs::Error>> allocate(int64_t offset, size_t size) override;

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	uint64_t _offset;
};

struct MemoryNode final : Node {
	friend struct MemoryFile;

	MemoryNode(Superblock *superblock);

	VfsType getType() override {
		return VfsType::regular;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}
		auto file = smarter::make_shared<MemoryFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		MemoryFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}

	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "\e[31mposix: Fix tmpfs getStats() in MemoryNode\e[39m" << std::endl;
		FileStats stats{};
		stats.inodeNumber = inodeNumber();
		stats.fileSize = _fileSize;
		stats.numLinks = numLinks();
		stats.mode = mode();
		stats.uid = uid();
		stats.gid = gid();
		stats.atimeSecs = atime().tv_sec;
		stats.atimeNanos = atime().tv_nsec;
		stats.mtimeSecs = mtime().tv_sec;
		stats.mtimeNanos = mtime().tv_nsec;
		stats.ctimeSecs = ctime().tv_sec;
		stats.ctimeNanos = ctime().tv_nsec;
		co_return stats;
	}

private:
	void _resizeFile(size_t new_size) {
		_fileSize = new_size;

		size_t aligned_size = (new_size + 0xFFF) & ~size_t(0xFFF);
		if(aligned_size <= _areaSize)
			return;

		if(_memory) {
			HEL_CHECK(helResizeMemory(_memory.getHandle(), aligned_size));
		}else{
			HelHandle handle;
			HEL_CHECK(helAllocateMemory(aligned_size, 0, nullptr, &handle));
			_memory = helix::UniqueDescriptor{handle};
		}

		_mapping = helix::Mapping{_memory, 0, aligned_size};
		_areaSize = aligned_size;
	}

	helix::UniqueDescriptor _memory;
	helix::Mapping _mapping;
	size_t _areaSize;
	size_t _fileSize;
};

struct Superblock final : FsSuperblock {
	FutureMaybe<std::shared_ptr<FsNode>> createRegular(Process *) override {
		auto node = std::make_shared<MemoryNode>(this);
		co_return std::move(node);
	}

	FutureMaybe<std::shared_ptr<FsNode>> createSocket() override {
		auto node = std::make_shared<SocketNode>(this);
		co_return std::move(node);
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>> rename(FsLink *src_fs_link,
			FsNode *dest_fs_dir, std::string dest_name) override {
		auto src_link = static_cast<Link *>(src_fs_link);
		auto dest_dir = static_cast<DirectoryNode *>(dest_fs_dir);

		auto src_dir = static_cast<DirectoryNode *>(src_link->getOwner().get());
		auto it = src_dir->_entries.find(src_link->getName());
		if(it == src_dir->_entries.end() || it->get() != src_link)
			co_return Error::alreadyExists;

		// Unlink an existing link if such a link exists.
		if(auto dest_it = dest_dir->_entries.find(dest_name);
				dest_it != dest_dir->_entries.end())
			dest_dir->_entries.erase(dest_it);

		auto new_link = std::make_shared<Link>(dest_dir->shared_from_this(),
				std::move(dest_name), src_link->getTarget());
		src_dir->_entries.erase(it);
		dest_dir->_entries.insert(new_link);
		co_return new_link;
	}

	int64_t allocateInode() {
		return _inodeCounter++;
	}

private:
	int64_t _inodeCounter = 1;
};

// ----------------------------------------------------------------------------
// MemoryNode and MemoryFile implementation.
// ----------------------------------------------------------------------------

MemoryNode::MemoryNode(Superblock *superblock)
: Node{superblock}, _areaSize{0}, _fileSize{0} { }

void MemoryFile::handleClose() {
	_cancelServe.cancel();
}

async::result<frg::expected<Error, off_t>>
MemoryFile::seek(off_t delta, VfsSeek whence) {
	if(whence == VfsSeek::absolute) {
		_offset = delta;
	}else if(whence == VfsSeek::relative){
		_offset += delta;
	}else if(whence == VfsSeek::eof) {
		assert(whence == VfsSeek::eof);
		auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());
		_offset += delta + node->_fileSize;
	}
	co_return _offset;
}

async::result<frg::expected<Error, size_t>>
MemoryFile::readSome(Process *, void *buffer, size_t max_length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	if(!(_offset <= node->_fileSize))
		co_return 0;
	auto chunk = std::min(node->_fileSize - _offset, max_length);

	memcpy(buffer, reinterpret_cast<char *>(node->_mapping.get()) + _offset, chunk);
	_offset += chunk;

	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
MemoryFile::writeAll(Process *, const void *buffer, size_t length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	if(_offset + length > node->_fileSize)
		node->_resizeFile(_offset + length);

	memcpy(reinterpret_cast<char *>(node->_mapping.get()) + _offset, buffer, length);
	_offset += length;
	co_return length;
}

async::result<frg::expected<Error, size_t>>
MemoryFile::pread(Process *, int64_t offset, void *buffer, size_t length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	if(static_cast<size_t>(offset) >= node->_fileSize)
		co_return 0;
	auto chunk = std::min(node->_fileSize - offset, length);

	memcpy(buffer, reinterpret_cast<char *>(node->_mapping.get()) + offset, chunk);

	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
MemoryFile::pwrite(Process *, int64_t offset, const void *buffer, size_t length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	if(offset + length > node->_fileSize)
		node->_resizeFile(offset + length);

	memcpy(reinterpret_cast<char *>(node->_mapping.get()) + offset, buffer, length);
	co_return length;
}

async::result<frg::expected<protocols::fs::Error>>
MemoryFile::truncate(size_t size) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	node->_resizeFile(size);
	co_return {};
}

async::result<frg::expected<protocols::fs::Error>>
MemoryFile::allocate(int64_t offset, size_t size) {
	assert(!offset);

	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	// TODO: Careful about overflow.
	if(offset + size <= node->_fileSize)
		co_return {};
	node->_resizeFile(offset + size);
	co_return {};
}

FutureMaybe<helix::UniqueDescriptor>
MemoryFile::accessMemory() {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());
	co_return node->_memory.dup();
}

// ----------------------------------------------------------------------------

InheritedNode::InheritedNode(Superblock *superblock, std::string path)
: Node{superblock}, _path{std::move(path)} { }

// ----------------------------------------------------------------------------
// DirectoryNode and DirectoryFile implementation.
// ----------------------------------------------------------------------------

void DirectoryFile::serve(smarter::shared_ptr<DirectoryFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

void DirectoryFile::handleClose() {
	_cancelServe.cancel();
}

DirectoryFile::DirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
: File{StructName::get("tmpfs.dir"), std::move(mount), std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())},
		_iter{_node->_entries.begin()} { }

// TODO: This iteration mechanism only works as long as _iter is not concurrently deleted.
async::result<ReadEntriesResult>
DirectoryFile::readEntries() {
	if(_iter != _node->_entries.end()) {
		auto name = (*_iter)->getName();
		_iter++;
		co_return name;
	}else{
		co_return std::nullopt;
	}
}

helix::BorrowedDescriptor DirectoryFile::getPassthroughLane() {
	return _passthrough;
}

// ----------------------------------------------------------------------------
// Node implementation.
// ----------------------------------------------------------------------------

Node::Node(Superblock *superblock, FsNode::DefaultOps default_ops)
: FsNode{superblock, default_ops} {
	_inodeNumber = superblock->allocateInode();
}

// ----------------------------------------------------------------------------
// SymlinkNode implementation.
// ----------------------------------------------------------------------------

SymlinkNode::SymlinkNode(Superblock *superblock, std::string link)
: Node{superblock}, _link(std::move(link)) { }

// ----------------------------------------------------------------------------
// DeviceNode implementation.
// ----------------------------------------------------------------------------

DeviceNode::DeviceNode(Superblock *superblock, VfsType type, DeviceId id)
: Node{superblock}, _type(type), _id(id) {
	assert(type == VfsType::charDevice || type == VfsType::blockDevice);
}

// ----------------------------------------------------------------------------
// SocketNode implementation.
// ----------------------------------------------------------------------------

SocketNode::SocketNode(Superblock *superblock)
: Node{superblock} { }

// ----------------------------------------------------------------------------
// DirectoryNode implementation.
// ----------------------------------------------------------------------------

std::shared_ptr<Link> DirectoryNode::createRootDirectory(Superblock *superblock) {
	auto node = std::make_shared<DirectoryNode>(superblock);
	auto the_node = node.get();
	auto link = std::make_shared<Link>(std::move(node));
	the_node->_treeLink = link;
	return link;
}

DirectoryNode::DirectoryNode(Superblock *superblock)
: Node{superblock, FsNode::defaultSupportsObservers} { }

async::result<std::variant<Error, std::shared_ptr<FsLink>>>
DirectoryNode::mkdir(std::string name) {
	if(!(_entries.find(name) == _entries.end()))
		co_return Error::alreadyExists;
	auto node = std::make_shared<DirectoryNode>(static_cast<Superblock *>(superblock()));
	auto the_node = node.get();
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	the_node->_treeLink = link;
	_entries.insert(link);
	notifyObservers(FsObserver::createEvent, name, 0);
	co_return link;
}

async::result<std::variant<Error, std::shared_ptr<FsLink>>>
DirectoryNode::symlink(std::string name, std::string path) {
	if(!(_entries.find(name) == _entries.end()))
		co_return Error::alreadyExists;
	auto node = std::make_shared<SymlinkNode>(static_cast<Superblock *>(superblock()),
			std::move(path));
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	co_return link;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
DirectoryNode::mkdev(std::string name, VfsType type, DeviceId id) {
	if(!(_entries.find(name) == _entries.end()))
		co_return Error::alreadyExists;
	auto node = std::make_shared<DeviceNode>(static_cast<Superblock *>(superblock()),
			type, id);
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	notifyObservers(FsObserver::createEvent, name, 0);
	co_return link;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
DirectoryNode::mkfifo(std::string name, mode_t mode) {
	if(!(_entries.find(name) == _entries.end()))
		co_return Error::alreadyExists;
	auto node = std::make_shared<FifoNode>(static_cast<Superblock *>(superblock()), mode);
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	notifyObservers(FsObserver::createEvent, name, 0);
	co_return link;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>> DirectoryNode::mksocket(std::string name) {
	if(!(_entries.find(name) == _entries.end()))
		co_return Error::alreadyExists;
	auto node = std::make_shared<SocketNode>(static_cast<Superblock *>(superblock()));
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	notifyObservers(FsObserver::createEvent, name, 0);
	co_return link;
}


// TODO: File system should not have global superblocks.
static Superblock globalSuperblock;

} // anonymous namespace

// Ironically, this function does not create a MemoryNode.
std::shared_ptr<FsNode> createMemoryNode(std::string path) {
	return std::make_shared<InheritedNode>(&globalSuperblock, std::move(path));
}

std::shared_ptr<FsLink> createRoot() {
	return DirectoryNode::createRootDirectory(&globalSuperblock);
}

} // namespace tmp_fs
