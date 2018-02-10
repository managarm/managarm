
#include <string.h>

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
// DirectoryFile implementation.
// ----------------------------------------------------------------------------

// TODO: This iteration mechanism only works as long as _iter is not concurrently deleted.
COFIBER_ROUTINE(async::result<protocols::fs::ReadEntriesResult>,
DirectoryFile::ptReadEntries(std::shared_ptr<void> object), ([=] {
	auto self = static_cast<DirectoryFile *>(object.get());
	if(self->_iter != self->_node->_entries.end()) {
		auto name = (*self->_iter)->getName();
		self->_iter++;
		COFIBER_RETURN(name);
	}else{
		COFIBER_RETURN(std::nullopt);
	}
}))

void DirectoryFile::serve(std::shared_ptr<DirectoryFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	protocols::fs::servePassthrough(std::move(lane), file,
			&fileOperations);
}

DirectoryFile::DirectoryFile(std::shared_ptr<FsLink> link)
: ProperFile{std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())},
		_iter{_node->_entries.begin()} { }

FutureMaybe<size_t> DirectoryFile::readSome(void *data, size_t max_length) {
	throw std::runtime_error("sysfs: DirectoryFile::readSome() is missing");
}

helix::BorrowedDescriptor DirectoryFile::getPassthroughLane() {
	return _passthrough;
}

// ----------------------------------------------------------------------------
// Link implementation.
// ----------------------------------------------------------------------------

Link::Link(std::shared_ptr<FsNode> owner, std::string name, std::shared_ptr<FsNode> target)
: owner(std::move(owner)), name(std::move(name)), target(std::move(target)) { }

std::shared_ptr<FsNode> Link::getOwner() {
	return owner;
}

std::string Link::getName() {
	return name;
}

std::shared_ptr<FsNode> Link::getTarget() {
	return target;
}

// ----------------------------------------------------------------------------
// SymlinkNode implementation.
// ----------------------------------------------------------------------------

VfsType SymlinkNode::getType() {
	return VfsType::symlink;
}

FileStats SymlinkNode::getStats() {
	std::cout << "\e[31mposix: Fix sysfs SymlinkNode::getStats()\e[39m" << std::endl;
	return FileStats{};
}

COFIBER_ROUTINE(FutureMaybe<std::string>, SymlinkNode::readSymlink(), ([=] {
	COFIBER_RETURN("../../devices/card0");
}))

// ----------------------------------------------------------------------------
// DirectoryNode implementation.
// ----------------------------------------------------------------------------

std::shared_ptr<Link> DirectoryNode::directMklink(std::string name) {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<SymlinkNode>();
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	return link;
}

std::shared_ptr<Link> DirectoryNode::directMkdir(std::string name) {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<DirectoryNode>();
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	return link;
}

VfsType DirectoryNode::getType() {
	return VfsType::directory;
}

FileStats DirectoryNode::getStats() {
	std::cout << "\e[31mposix: Fix sysfs Directory::getStats()\e[39m" << std::endl;
	return FileStats{};
}

COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<ProperFile>>,
		DirectoryNode::open(std::shared_ptr<FsLink> link), ([=] {
	auto file = std::make_shared<DirectoryFile>(std::move(link));
	DirectoryFile::serve(file);
	COFIBER_RETURN(std::move(file));
}))

COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>,
		DirectoryNode::getLink(std::string name), ([=] {
	auto it = _entries.find(name);
	if(it != _entries.end())
		COFIBER_RETURN(*it);
	COFIBER_RETURN(nullptr); // TODO: Return an error code.
}))

// ----------------------------------------------------------------------------
// Object implementation
// ----------------------------------------------------------------------------

Object::Object(std::shared_ptr<Object> parent, std::string name)
: _parent{std::move(parent)}, _name{std::move(name)} { }

void Object::createSymlink(std::string name, std::shared_ptr<Object> target) {
	assert(_dirLink);
	auto dir = static_cast<DirectoryNode *>(_dirLink->getTarget().get());
	dir->directMklink(std::move(name));
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

// ----------------------------------------------------------------------------
// Free functions.
// ----------------------------------------------------------------------------

std::shared_ptr<FsLink> createRoot() {
	auto node = std::make_shared<DirectoryNode>();
	return std::make_shared<Link>(nullptr, std::string{}, std::move(node));
}

} // namespace sysfs

std::shared_ptr<FsLink> getSysfs() {
	static std::shared_ptr<FsLink> sysfs = sysfs::createRoot();
	return sysfs;
}

