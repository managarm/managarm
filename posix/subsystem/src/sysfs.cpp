
#include <string.h>

#include "clock.hpp"
#include "common.hpp"
#include "sysfs.hpp"

#include <bitset>

namespace sysfs {

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
// Attribute implementation.
// ----------------------------------------------------------------------------

async::result<Error> Attribute::store(Object *, std::string) {
	co_return Error::illegalOperationTarget;
}

async::result<frg::expected<Error, helix::UniqueDescriptor>> Attribute::accessMemory(Object *) {
	co_return Error::noBackingDevice;
}

// ----------------------------------------------------------------------------
// AttributeFile implementation.
// ----------------------------------------------------------------------------

void AttributeFile::serve(smarter::shared_ptr<AttributeFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

AttributeFile::AttributeFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
: File{StructName::get("sysfs.attr"), std::move(mount), std::move(link)},
		_cached{false}, _offset{0} { }

void AttributeFile::handleClose() {
	_cancelServe.cancel();
}

async::result<frg::expected<Error, off_t>> AttributeFile::seek(off_t offset, VfsSeek whence) {
	// TODO: it's unclear whether we should allow seeks past the end.
	// TODO: re-cache the file for seeks to zero.
	if(whence == VfsSeek::relative)
		_offset = _offset + offset;
	else if(whence == VfsSeek::absolute)
		_offset = offset;
	else if(whence == VfsSeek::eof)
		// TODO: Unimplemented!
		assert(!"unimplemented");
	co_return _offset;
}

async::result<frg::expected<Error, size_t>>
AttributeFile::readSome(Process *process, void *data, size_t max_length) {
	auto ret = co_await pread(process, _offset, data, max_length);

	if(ret)
		_offset += ret.value();

	co_return ret;
}

async::result<frg::expected<Error, size_t>>
AttributeFile::pread(Process *, int64_t offset, void *buffer, size_t length) {
	assert(length > 0);

	if(!_cached) {
		auto node = static_cast<AttributeNode *>(associatedLink()->getTarget().get());
		if(auto res = co_await node->_attr->show(node->_object); res) {
			_buffer = res.value();
			_cached = true;
		} else
			co_return res.error();
	}

	if(offset >= 0 && static_cast<size_t>(offset) >= _buffer.size())
		co_return 0;
	size_t chunk = std::min(_buffer.size() - offset, length);
	memcpy(buffer, _buffer.data() + offset, chunk);
	co_return chunk;
}

async::result<frg::expected<Error, size_t>>
AttributeFile::writeAll(Process *, const void *data, size_t length)  {
	assert(length > 0);

	auto node = static_cast<AttributeNode *>(associatedLink()->getTarget().get());
	auto err = co_await node->_attr->store(node->_object,
			std::string{reinterpret_cast<const char *>(data), length});

	if(err != Error::success)
		co_return err;

	co_return length;
}

FutureMaybe<helix::UniqueDescriptor> AttributeFile::accessMemory() {
	auto node = static_cast<AttributeNode *>(associatedLink()->getTarget().get());

	auto res = co_await node->_attr->accessMemory(node->_object);
	assert(res);

	co_return std::move(res.value());
}

helix::BorrowedDescriptor AttributeFile::getPassthroughLane() {
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
: File{StructName::get("sysfs.dir"), std::move(mount), std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())},
		_iter{_node->_entries.begin()} { }

void DirectoryFile::handleClose() {
	_cancelServe.cancel();
}

// TODO: Verify that this is correct
async::result<frg::expected<Error, off_t>> DirectoryFile::seek(off_t, VfsSeek) {
	co_return Error::illegalArguments;
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
// AttributeNode implementation.
// ----------------------------------------------------------------------------

AttributeNode::AttributeNode(Object *object, Attribute *attr)
: _object{object}, _attr{attr} { }

VfsType AttributeNode::getType() {
	return VfsType::regular;
}

async::result<frg::expected<Error, FileStats>> AttributeNode::getStats() {
	// TODO: Store a file creation time.
	auto now = clk::getRealtime();

	FileStats stats;
	stats.inodeNumber = 0; // FIXME
	stats.numLinks = 1;
	stats.fileSize = _attr->size();
	stats.mode = _attr->writable() ? 0666 : 0444; // TODO: Some files can be written.
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
AttributeNode::open(std::shared_ptr<MountView> mount,
		std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: sysfs AttributeNode open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	auto file = smarter::make_shared<AttributeFile>(std::move(mount), std::move(link));
	file->setupWeakFile(file);
	AttributeFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

// ----------------------------------------------------------------------------
// SymlinkNode implementation.
// ----------------------------------------------------------------------------

SymlinkNode::SymlinkNode(std::weak_ptr<Object> target)
: _target{std::move(target)} { }

VfsType SymlinkNode::getType() {
	return VfsType::symlink;
}

async::result<frg::expected<Error, FileStats>> SymlinkNode::getStats() {
	auto fs = FileStats{};
	fs.numLinks = 1;
	fs.mode = 0777;
	co_return fs;
}

expected<std::string> SymlinkNode::readSymlink(FsLink *link, Process *) {
	auto object = _target.lock();
	assert(object);

	std::string path;

	// Walk from the target to the root to discover the path.
	auto ref = object->directoryNode();
	while(true) {
		auto link = ref->treeLink();
		if(!link->getOwner())
			break;
		path = path.empty() ? link->getName() : link->getName() + "/" + path;
		ref = std::static_pointer_cast<DirectoryNode>(link->getOwner());
	}

	// Walk from the symlink to the root to discover the number of ../ prefixes.
	ref = std::static_pointer_cast<DirectoryNode>(link->getOwner());
	while(true) {
		auto link = ref->treeLink();
		if(!link->getOwner())
			break;
		path = "../" + path;
		ref = std::static_pointer_cast<DirectoryNode>(link->getOwner());
	}

	co_return path;
}

// ----------------------------------------------------------------------------
// DirectoryNode implementation.
// ----------------------------------------------------------------------------

std::shared_ptr<Link> DirectoryNode::createRootDirectory() {
	auto node = std::make_shared<DirectoryNode>();
	auto the_node = node.get();
	auto link = std::make_shared<Link>(std::move(node));
	the_node->_treeLink = link.get();
	return link;
}

DirectoryNode::DirectoryNode()
: _treeLink{nullptr} { }

std::shared_ptr<Link> DirectoryNode::directMkattr(Object *object, Attribute *attr) {
	assert(_entries.find(attr->name()) == _entries.end());
	auto node = std::make_shared<AttributeNode>(object, attr);
	auto link = std::make_shared<Link>(shared_from_this(), attr->name(), std::move(node));
	_entries.insert(link);
	return link;
}

std::shared_ptr<Link> DirectoryNode::directMklink(std::string name, std::weak_ptr<Object> target) {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<SymlinkNode>(std::move(target));
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	return link;
}

std::shared_ptr<Link> DirectoryNode::directMkdir(std::string name) {
	auto preexisting = _entries.find(name);
	if(preexisting != _entries.end()) {
		return *preexisting;
	}
	auto node = std::make_shared<DirectoryNode>();
	auto the_node = node.get();
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	the_node->_treeLink = link.get();
	return link;
}

VfsType DirectoryNode::getType() {
	return VfsType::directory;
}

async::result<frg::expected<Error, FileStats>> DirectoryNode::getStats() {
	auto fs = FileStats{};
	fs.numLinks = 2;
	fs.fileSize = 0;
	fs.mode = 0755;
	fs.uid = 0;
	fs.gid = 0;
	co_return fs;
}

std::shared_ptr<FsLink> DirectoryNode::treeLink() {
	// TODO: Even the root should return a valid link.
	return _treeLink ? _treeLink->shared_from_this() : nullptr;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
DirectoryNode::open(std::shared_ptr<MountView> mount,
		std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: sysfs DirectoryNode open() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
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

// ----------------------------------------------------------------------------
// Attribute implementation
// ----------------------------------------------------------------------------

Attribute::Attribute(std::string name, bool writable)
: _name{std::move(name)}, _writable{writable} { }

Attribute::Attribute(std::string name, bool writable, size_t size)
: _size{size}, _name{std::move(name)}, _writable{writable} { }

// ----------------------------------------------------------------------------
// Object implementation
// ----------------------------------------------------------------------------

Object::Object(std::shared_ptr<Object> parent, std::string name)
: _parent{std::move(parent)}, _name{std::move(name)} { }

std::shared_ptr<DirectoryNode> Object::directoryNode() {
	return std::static_pointer_cast<DirectoryNode>(_dirLink->getTarget());
}

void Object::realizeAttribute(Attribute *attr) {
	assert(_dirLink);
	auto dir = static_cast<DirectoryNode *>(_dirLink->getTarget().get());
	dir->directMkattr(this, attr);
}

void Object::createSymlink(std::string name, std::shared_ptr<Object> target) {
	assert(_dirLink);
	auto dir = static_cast<DirectoryNode *>(_dirLink->getTarget().get());
	dir->directMklink(std::move(name), std::move(target));
}

std::optional<std::string> Object::getClassPath() {
	return std::nullopt;
}

void Object::addObject() {
	if(_parent) {
		assert(_parent->_dirLink);
		auto parent_dir = static_cast<DirectoryNode *>(_parent->_dirLink->getTarget().get());

		auto class_path = getClassPath();
		if(class_path.has_value())
			parent_dir = static_cast<DirectoryNode *>(parent_dir->directMkdir(class_path.value())->getTarget().get());

		_dirLink = parent_dir->directMkdir(_name);
	}else{
		auto parent_dir = static_cast<DirectoryNode *>(getSysfs()->getTarget().get());
		_dirLink = parent_dir->directMkdir(_name);
	}
}

} // namespace sysfs

std::shared_ptr<FsLink> getSysfs() {
	static std::shared_ptr<FsLink> sysfs = sysfs::DirectoryNode::createRootDirectory();
	return sysfs;
}

