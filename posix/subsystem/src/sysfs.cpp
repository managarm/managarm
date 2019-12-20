
#include <string.h>

#include "clock.hpp"
#include "common.hpp"
#include "device.hpp"
#include "sysfs.hpp"

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

async::result<void> Attribute::store(Object *object, std::string data) {
	// FIXME: Return an error to the caller.
	throw std::runtime_error("Attribute does not support store()");
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

expected<off_t> AttributeFile::seek(off_t offset, VfsSeek whence) {
	assert(whence == VfsSeek::relative && !offset);
	co_return _offset;
}

expected<size_t> AttributeFile::readSome(Process *, void *data, size_t max_length) {
	assert(max_length > 0);

	if(!_cached) {
		assert(!_offset);
		auto node = static_cast<AttributeNode *>(associatedLink()->getTarget().get());
		_buffer = co_await node->_attr->show(node->_object);
		_cached = true;
	}

	assert(_offset <= _buffer.size());
	size_t chunk = std::min(_buffer.size() - _offset, max_length);
	memcpy(data, _buffer.data() + _offset, chunk);
	_offset += chunk;
	co_return chunk;
}

FutureMaybe<void> AttributeFile::writeAll(Process *, const void *data, size_t length)  {
	assert(length > 0);

	auto node = static_cast<AttributeNode *>(associatedLink()->getTarget().get());
	co_await node->_attr->store(node->_object,
			std::string{reinterpret_cast<const char *>(data), length});
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

FutureMaybe<FileStats> AttributeNode::getStats() {
	// TODO: Store a file creation time.
	auto now = clk::getRealtime();
	
	FileStats stats;
	stats.inodeNumber = 0; // FIXME
	stats.numLinks = 1;
	stats.fileSize = 4096; // Same as in Linux.
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

FutureMaybe<SharedFilePtr> AttributeNode::open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	assert(!semantic_flags);

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

FutureMaybe<FileStats> SymlinkNode::getStats() {
	std::cout << "\e[31mposix: Fix sysfs SymlinkNode::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

expected<std::string> SymlinkNode::readSymlink(FsLink *link) {
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
	assert(_entries.find(name) == _entries.end());
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

FutureMaybe<FileStats> DirectoryNode::getStats() {
	std::cout << "\e[31mposix: Fix sysfs Directory::getStats()\e[39m" << std::endl;
	co_return FileStats{};
}

std::shared_ptr<FsLink> DirectoryNode::treeLink() {
	// TODO: Even the root should return a valid link.
	return _treeLink ? _treeLink->shared_from_this() : nullptr;
}

FutureMaybe<SharedFilePtr> DirectoryNode::open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	assert(!semantic_flags);

	auto file = smarter::make_shared<DirectoryFile>(std::move(mount), std::move(link));
	file->setupWeakFile(file);
	DirectoryFile::serve(file);
	co_return File::constructHandle(std::move(file));
}

FutureMaybe<std::shared_ptr<FsLink>> DirectoryNode::getLink(std::string name) {
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

void Object::addObject() {
	if(_parent) {
		assert(_parent->_dirLink);
		auto parent_dir = static_cast<DirectoryNode *>(_parent->_dirLink->getTarget().get());
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

