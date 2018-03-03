
#include <string.h>
#include <future>

#include <cofiber.hpp>
#include <cofiber/future.hpp>

#include "common.hpp"
#include "fs.hpp"

// --------------------------------------------------------
// FsNode implementation.
// --------------------------------------------------------

VfsType FsNode::getType() {
	throw std::runtime_error("getType() is not implemented for this FsNode");
}

FutureMaybe<FileStats> FsNode::getStats() {
	throw std::runtime_error("getStats() is not implemented for this FsNode");
}

std::shared_ptr<FsLink> FsNode::treeLink() {
	throw std::runtime_error("treeLink() is not implemented for this FsNode");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::getLink(std::string) {
	throw std::runtime_error("getLink() is not implemented for this FsNode");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::link(std::string, std::shared_ptr<FsNode>) {
	throw std::runtime_error("link() is not implemented for this FsNode");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::mkdir(std::string) {
	throw std::runtime_error("mkdir() is not implemented for this FsNode");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::symlink(std::string, std::string) {
	throw std::runtime_error("symlink() is not implemented for this FsNode");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::mkdev(std::string, VfsType, DeviceId) {
	throw std::runtime_error("mkdev() is not implemented for this FsNode");
}

FutureMaybe<void> FsNode::unlink(std::string) {
	throw std::runtime_error("unlink() is not implemented for this FsNode");
}

FutureMaybe<smarter::shared_ptr<File, FileHandle>>
FsNode::open(std::shared_ptr<FsLink>, SemanticFlags) {
	throw std::runtime_error("open() is not implemented for this FsNode");
}

expected<std::string> FsNode::readSymlink(FsLink *link) {
	async::promise<std::variant<Error, std::string>> p;
	p.set_value(Error::illegalOperationTarget);
	return p.async_get();
}

DeviceId FsNode::readDevice() {
	throw std::runtime_error("readDevice() is not implemented for this FsNode");
}

