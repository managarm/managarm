
#include <string.h>
#include <future>

#include <cofiber.hpp>
#include <cofiber/future.hpp>

#include "common.hpp"
#include "fs.hpp"

// --------------------------------------------------------
// Node implementation.
// --------------------------------------------------------

VfsType Node::getType() {
	throw std::runtime_error("Not implemented");
}

FileStats Node::getStats() {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<Link>> Node::getLink(std::string) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<Link>> Node::link(std::string, std::shared_ptr<Node>) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<Link>> Node::mkdir(std::string) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<Link>> Node::symlink(std::string, std::string) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<Link>> Node::mkdev(std::string, VfsType, DeviceId) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::shared_ptr<ProperFile>> Node::open(std::shared_ptr<Link>) {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::string> Node::readSymlink() {
	throw std::runtime_error("Not implemented");
}

DeviceId Node::readDevice() {
	throw std::runtime_error("Not implemented");
}

