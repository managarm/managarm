
#include <string.h>
#include <future>


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

void FsNode::addObserver(std::shared_ptr<FsObserver> observer) {
	if(!(_defaultOps & defaultSupportsObservers))
		std::cout << "\e[31m" "posix: FsNode does not support observers" "\e[39m" << std::endl;

	// TODO: For increased efficiency, Observers could be stored in an intrusive list.
	auto borrowed = observer.get();
	auto [it, inserted] = _observers.insert({borrowed, std::move(observer)});
	(void)it;
	assert(inserted); // Registering observers twice is an error.
}

void FsNode::removeObserver(FsObserver *observer) {
	auto it = _observers.find(observer);
	assert(it != _observers.end());
	_observers.erase(it);
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::getLink(std::string) {
	throw std::runtime_error("getLink() is not implemented for this FsNode");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::link(std::string, std::shared_ptr<FsNode>) {
	throw std::runtime_error("link() is not implemented for this FsNode");
}

async::result<std::variant<Error, std::shared_ptr<FsLink>>> FsNode::mkdir(std::string) {
	throw std::runtime_error("mkdir() is not implemented for this FsNode");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::symlink(std::string, std::string) {
	throw std::runtime_error("symlink() is not implemented for this FsNode");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::mkdev(std::string, VfsType, DeviceId) {
	throw std::runtime_error("mkdev() is not implemented for this FsNode");
}

FutureMaybe<std::shared_ptr<FsLink>> FsNode::mkfifo(std::string, mode_t) {
	throw std::runtime_error("mkfifo() is not implemented for this FsNode");
}


FutureMaybe<void> FsNode::unlink(std::string) {
	throw std::runtime_error("unlink() is not implemented for this FsNode");
}

FutureMaybe<smarter::shared_ptr<File, FileHandle>>
FsNode::open(std::shared_ptr<MountView>, std::shared_ptr<FsLink>, SemanticFlags) {
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

async::result<Error> FsNode::chmod(int mode) {
	std::cout << "\e[31m" "posix: chmod() is not implemented for this FsNode" "\e[39m" << std::endl;
	co_return Error::accessDenied;
}

void FsNode::notifyObservers(uint32_t events, const std::string &name, uint32_t cookie) {
	assert(_defaultOps & defaultSupportsObservers);
	for(const auto &[borrowed, observer] : _observers) {
		borrowed->observeNotification(events, name, cookie);
		(void)observer;
	}
}

