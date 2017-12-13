
#include <string.h>
#include <future>

#include <cofiber.hpp>
#include <cofiber/future.hpp>

#include "common.hpp"
#include "fs.hpp"

// --------------------------------------------------------
// Link implementation.
// --------------------------------------------------------

namespace {
	struct RootLink : Link {
	private:
		std::shared_ptr<Node> getOwner() override {
			return std::shared_ptr<Node>{};
		}

		std::string getName() override {
			assert(!"No associated name");
		}

		std::shared_ptr<Node> getTarget() override {
			return _target;
		}

	public:
		RootLink(std::shared_ptr<Node> target)
		: _target{std::move(target)} { }

	private:
		std::shared_ptr<Node> _target;
	};
}

std::shared_ptr<Link> createRootLink(std::shared_ptr<Node> target) {
	return std::make_shared<RootLink>(std::move(target));
}

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

FutureMaybe<std::shared_ptr<File>> Node::open() {
	throw std::runtime_error("Not implemented");
}

FutureMaybe<std::string> Node::readSymlink() {
	throw std::runtime_error("Not implemented");
}

DeviceId Node::readDevice() {
	throw std::runtime_error("Not implemented");
}

