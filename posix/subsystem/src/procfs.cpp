#include <string.h>
#include <sstream>
#include <iomanip>

#include "clock.hpp"
#include "common.hpp"
#include "device.hpp"
#include "procfs.hpp"
#include "process.hpp"

#include <bitset>

namespace procfs {

SuperBlock procfs_superblock;

// ----------------------------------------------------------------------------
// LinkCompare implementation.
// ----------------------------------------------------------------------------

bool LinkCompare::operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const {
	return a->getName() < b->getName();
}

bool LinkCompare::operator() (const std::shared_ptr<Link> &link, const std::string &name) const {
	return link->getName() < name;
}

bool LinkCompare::operator() (const std::string &name, const std::shared_ptr<Link> &link) const {
	return name < link->getName();
}

// ----------------------------------------------------------------------------
// RegularFile implementation.
// ----------------------------------------------------------------------------

void RegularFile::serve(smarter::shared_ptr<RegularFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

RegularFile::RegularFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
: File{StructName::get("procfs.attr"), std::move(mount), std::move(link)},
		_cached{false}, _offset{0} { }

void RegularFile::handleClose() {
	_cancelServe.cancel();
}

async::result<frg::expected<Error, off_t>> RegularFile::seek(off_t offset, VfsSeek whence) {
	assert(whence == VfsSeek::relative && !offset);
	co_return _offset;
}

async::result<frg::expected<Error, size_t>>
RegularFile::readSome(Process *, void *data, size_t max_length) {
	assert(max_length > 0);

	if(!_cached) {
		assert(!_offset);
		auto node = static_cast<RegularNode *>(associatedLink()->getTarget().get());
		_buffer = co_await node->show();
		_cached = true;
	}

	assert(_offset <= _buffer.size());
	size_t chunk = std::min(_buffer.size() - _offset, max_length);
	memcpy(data, _buffer.data() + _offset, chunk);
	_offset += chunk;
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
RegularFile::writeAll(Process *, const void *data, size_t length) {
	assert(length > 0);

	auto node = static_cast<RegularNode *>(associatedLink()->getTarget().get());
	co_await node->store(std::string{reinterpret_cast<const char *>(data), length});
	co_return length;
}

helix::BorrowedDescriptor RegularFile::getPassthroughLane() {
	return _passthrough;
}

// ----------------------------------------------------------------------------
// DirectoryFile implementation.
// ----------------------------------------------------------------------------

void DirectoryFile::serve(smarter::shared_ptr<DirectoryFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

DirectoryFile::DirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
: File{StructName::get("procfs.dir"), std::move(mount), std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())},
		_iter{_node->_entries.begin()} { }

void DirectoryFile::handleClose() {
	_cancelServe.cancel();
}

// TODO: This iteration mechanism only works as long as _iter is not concurrently deleted.
async::result<ReadEntriesResult> DirectoryFile::readEntries() {
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
// Link implementation.
// ----------------------------------------------------------------------------

Link::Link(std::shared_ptr<FsNode> target)
: _target{std::move(target)} { }

Link::Link(std::shared_ptr<FsNode> owner, std::string name, std::shared_ptr<FsNode> target)
: _owner{std::move(owner)}, _name{std::move(name)}, _target{std::move(target)} {
	assert(_owner);
	assert(!_name.empty());
}

std::shared_ptr<FsNode> Link::getOwner() {
	return _owner;
}

std::string Link::getName() {
	// The root link does not have a name.
	assert(_owner);
	return _name;
}

std::shared_ptr<FsNode> Link::getTarget() {
	return _target;
}

// ----------------------------------------------------------------------------
// RegularNode implementation.
// ----------------------------------------------------------------------------

RegularNode::RegularNode() = default;

VfsType RegularNode::getType() {
	return VfsType::regular;
}

async::result<frg::expected<Error, FileStats>> RegularNode::getStats() {
	// TODO: Store a file creation time.
	auto now = clk::getRealtime();

	FileStats stats;
	stats.inodeNumber = 0; // FIXME
	stats.numLinks = 1;
	stats.fileSize = 4096; // Same as in Linux.
	stats.mode = 0666; // TODO: Some files can be written.
	stats.uid = 0;
	stats.gid = 0;
	stats.atimeSecs = now.tv_sec;
	stats.atimeNanos = now.tv_nsec;
	stats.mtimeSecs = now.tv_sec;
	stats.mtimeNanos = now.tv_nsec;
	stats.ctimeSecs = now.tv_sec;
	stats.ctimeNanos = now.tv_nsec;
	co_return stats;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
RegularNode::open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	auto file = smarter::make_shared<RegularFile>(std::move(mount), std::move(link));
	file->setupWeakFile(file);
	RegularFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

FutureMaybe<std::shared_ptr<FsNode>> SuperBlock::createRegular() {
	co_return nullptr;
}

FutureMaybe<std::shared_ptr<FsNode>> SuperBlock::createSocket() {
	co_return nullptr;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
SuperBlock::rename(FsLink *source, FsNode *directory, std::string name) {
	co_return Error::noSuchFile;
};

// ----------------------------------------------------------------------------
// DirectoryNode implementation.
// ----------------------------------------------------------------------------

std::shared_ptr<Link> DirectoryNode::createRootDirectory() {
	auto node = std::make_shared<DirectoryNode>();
	auto the_node = node.get();
	auto link = std::make_shared<Link>(std::move(node));
	the_node->_treeLink = link.get();

	auto self_link = std::make_shared<Link>(the_node->shared_from_this(), "self", std::make_shared<SelfLink>());
	the_node->_entries.insert(std::move(self_link));
	return link;
}

DirectoryNode::DirectoryNode()
: FsNode{&procfs_superblock}, _treeLink{nullptr} { }

std::shared_ptr<Link> DirectoryNode::directMkregular(std::string name,
		std::shared_ptr<RegularNode> regular) {
	assert(_entries.find(name) == _entries.end());
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(regular));
	_entries.insert(link);
	return link;
}

std::shared_ptr<Link> DirectoryNode::directMkdir(std::string name) {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<DirectoryNode>();
	auto the_node = node.get();
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	the_node->_treeLink = link.get();
	return link;
}

std::shared_ptr<Link> DirectoryNode::directMknode(std::string name, std::shared_ptr<FsNode> node) {
	assert(_entries.find(name) == _entries.end());
	auto link = std::make_shared<Link>(shared_from_this(), name, std::move(node));
	_entries.insert(link);
	return link;
}

std::shared_ptr<Link> DirectoryNode::createProcDirectory(std::string name,
		Process *process) {
	auto link = directMkdir(name);
	auto proc_dir = static_cast<DirectoryNode*>(link->getTarget().get());

	proc_dir->directMknode("exe", std::make_shared<ExeLink>(process));
	proc_dir->directMknode("root", std::make_shared<RootLink>(process));
	proc_dir->directMkregular("maps", std::make_shared<MapNode>(process));
	proc_dir->directMkregular("comm", std::make_shared<CommNode>(process));

	return link;
}

VfsType DirectoryNode::getType() {
	return VfsType::directory;
}

async::result<frg::expected<Error, std::shared_ptr<FsLink>>> DirectoryNode::link(std::string name,
		std::shared_ptr<FsNode> target) {
	co_return Error::noSuchFile;
}

async::result<frg::expected<Error, FileStats>> DirectoryNode::getStats() {
	std::cout << "\e[31mposix: Fix procfs Directory::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

std::shared_ptr<FsLink> DirectoryNode::treeLink() {
	// TODO: Even the root should return a valid link.
	return _treeLink ? _treeLink->shared_from_this() : nullptr;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
DirectoryNode::open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
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

async::result<frg::expected<Error, std::shared_ptr<FsLink>>> DirectoryNode::getLink(std::string name) {
	auto it = _entries.find(name);
	if(it != _entries.end())
		co_return *it;
	co_return nullptr; // TODO: Return an error code.
}

async::result<frg::expected<Error>> DirectoryNode::unlink(std::string name) {
	auto it = _entries.find(name);
	if (it == _entries.end())
		co_return Error::noSuchFile;
	_entries.erase(it);
	co_return frg::expected<Error>{};
}

VfsType SelfLink::getType() {
	return VfsType::symlink;
}

expected<std::string> SelfLink::readSymlink(FsLink *link, Process *process) {
	co_return "/proc/" + std::to_string(process->pid());
}

async::result<frg::expected<Error, FileStats>> SelfLink::getStats() {
	std::cout << "\e[31mposix: Fix procfs SelfLink::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

VfsType ExeLink::getType() {
	return VfsType::symlink;
}

expected<std::string> ExeLink::readSymlink(FsLink *link, Process *process) {
	co_return _process->path();
}

async::result<frg::expected<Error, FileStats>> ExeLink::getStats() {
	std::cout << "\e[31mposix: Fix procfs ExeLink::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

async::result<std::string> MapNode::show() {
	auto vmContext = _process->vmContext();
	std::stringstream stream;
	for (auto area : *vmContext) {
		stream << std::hex << area.baseAddress();
		stream << "-";
		stream << std::hex << area.baseAddress() + area.size();
		stream << " ";
		stream << (area.isReadable() ? "r" : "-");
		stream << (area.isWritable() ? "w" : "-");
		stream << (area.isExecutable() ? "x" : "-");
		stream << (area.isPrivate() ? "p" : "-");
		stream << " ";
		auto backingFile = area.backingFile();
		if(backingFile && backingFile->associatedLink() && backingFile->associatedMount()) {
			stream << std::setfill('0') << std::setw(8) << area.backingFileOffset();
			stream << " ";
			auto fsNode = backingFile->associatedLink()->getTarget();
			ViewPath viewPath = {backingFile->associatedMount(), backingFile->associatedLink()};
			auto fileStats = co_await fsNode->getStats();
			DeviceId deviceId{};
			if (fsNode->getType() == VfsType::charDevice || fsNode->getType() == VfsType::blockDevice)
				deviceId = fsNode->readDevice();
			assert(fileStats);

			stream << std::dec << std::setfill('0') << std::setw(2) << deviceId.first << ":" << deviceId.second;
			stream << " ";
			stream << std::setw(0) << fileStats.value().inodeNumber;
			stream << "    ";
			stream << viewPath.getPath(_process->fsContext()->getRoot());
		} else {
			// TODO: In the case of memfd files, show the name here.
			stream << "00000000 00:00 0";
		}
		stream << "\n";
	}
	co_return stream.str();
}

async::result<void> MapNode::store(std::string) {
	// TODO: proper error reporting.
	std::cout << "posix: Can't store to a /proc/maps file" << std::endl;
	co_return;
}

async::result<std::string> CommNode::show() {
	// See man 5 proc for more details.
	// Based on the man page from Linux man-pages 6.01, updated on 2022-10-09.
	std::stringstream stream;
	stream << _process->name() << "\n";
	co_return stream.str();
}

async::result<void> CommNode::store(std::string name) {
	_process->setName(name);
	co_return;
}

VfsType RootLink::getType() {
	return VfsType::symlink;
}

expected<std::string> RootLink::readSymlink(FsLink *link, Process *process) {
	co_return _process->fsContext()->getRoot().getPath(_process->fsContext()->getRoot());
}

async::result<frg::expected<Error, FileStats>> RootLink::getStats() {
	std::cout << "\e[31mposix: Fix procfs RootLink::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

} // namespace procfs

std::shared_ptr<FsLink> getProcfs() {
	static std::shared_ptr<FsLink> procfs = procfs::DirectoryNode::createRootDirectory();
	return procfs;
}
