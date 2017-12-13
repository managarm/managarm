
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
	throw std::runtime_error("Not implemented");
}

FileStats FsNode::getStats() {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::getLink(std::string) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::link(std::string, std::shared_ptr<FsNode>) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::mkdir(std::string) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::symlink(std::string, std::string) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::mkdev(std::string, VfsType, DeviceId) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<ProperFile>> FsNode::open(std::shared_ptr<FsLink>) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::string> FsNode::readSymlink() {
	throw std::runtime_error("Not implemented");
}

DeviceId FsNode::readDevice() {
	throw std::runtime_error("Not implemented");
}

