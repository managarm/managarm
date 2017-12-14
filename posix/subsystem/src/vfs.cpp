
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <future>

#include <cofiber.hpp>
#include <cofiber/future.hpp>

#include "common.hpp"
#include "fs.pb.h"
#include "vfs.hpp"
#include "device.hpp"
#include "tmp_fs.hpp"
#include "extern_fs.hpp"

HelHandle __mlibc_getPassthrough(int fd);

static bool debugResolve = false;

// --------------------------------------------------------
// MountView implementation.
// --------------------------------------------------------

std::shared_ptr<MountView> MountView::createRoot(std::shared_ptr<FsLink> origin) {
	return std::make_shared<MountView>(nullptr, nullptr, std::move(origin));
}

std::shared_ptr<FsLink> MountView::getAnchor() const {
	return _anchor;
}

std::shared_ptr<FsLink> MountView::getOrigin() const {
	return _origin;
}

void MountView::mount(std::shared_ptr<FsLink> anchor, std::shared_ptr<FsLink> origin) {
	_mounts.insert(std::make_shared<MountView>(shared_from_this(),
			std::move(anchor), std::move(origin)));
	// TODO: check insert return value
}

std::shared_ptr<MountView> MountView::getMount(std::shared_ptr<FsLink> link) const {
	auto it = _mounts.find(link);
	if(it == _mounts.end())
		return nullptr;
	return *it;
}

namespace {

std::shared_ptr<MountView> rootView;

} // anonymous namespace

COFIBER_ROUTINE(async::result<void>, populateRootView(), ([=] {
	// Create a tmpfs instance for the initrd.
	auto tree = tmp_fs::createRoot();
	rootView = MountView::createRoot(tree);

	COFIBER_AWAIT tree->getTarget()->mkdir("realfs");
	
	auto dev = COFIBER_AWAIT tree->getTarget()->mkdir("dev");
	rootView->mount(std::move(dev), getDevtmpfs());

	// Populate the tmpfs from the fs we are running on.
	std::vector<
		std::pair<
			std::shared_ptr<FsNode>,
			std::string
		>
	> stack;

	stack.push_back({tree->getTarget(), std::string{""}});
	
	while(!stack.empty()) {
		auto item = stack.back();
		stack.pop_back();

		auto dir_path = "/" + item.second;
		auto dir_fd = open(dir_path.c_str(), O_RDONLY);
		assert(dir_fd != -1);

		auto lane = helix::BorrowedLane{__mlibc_getPassthrough(dir_fd)};
		while(true) {
			helix::Offer offer;
			helix::SendBuffer send_req;
			helix::RecvInline recv_resp;

			managarm::fs::CntRequest req;
			req.set_req_type(managarm::fs::CntReqType::PT_READ_ENTRIES);

			auto ser = req.SerializeAsString();
			auto &&transmit = helix::submitAsync(lane, helix::Dispatcher::global(),
					helix::action(&offer, kHelItemAncillary),
					helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
					helix::action(&recv_resp));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(offer.error());
			HEL_CHECK(send_req.error());
			HEL_CHECK(recv_resp.error());

			managarm::fs::SvrResponse resp;
			resp.ParseFromArray(recv_resp.data(), recv_resp.length());
			if(resp.error() == managarm::fs::Errors::END_OF_FILE)
				break;
			assert(resp.error() == managarm::fs::Errors::SUCCESS);

			//std::cout << "posix: Importing " << item.second + "/" + resp.path() << std::endl;

			if(resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				auto link = COFIBER_AWAIT item.first->mkdir(resp.path());
				stack.push_back({link->getTarget(), item.second + "/" + resp.path()});
			}else{
				assert(resp.file_type() == managarm::fs::FileType::REGULAR);

				auto file_path = "/" + item.second + "/" + resp.path();
				auto node = tmp_fs::createMemoryNode(std::move(file_path));
				COFIBER_AWAIT item.first->link(resp.path(), node);
			}
		}
	}

	COFIBER_RETURN();
}))

#include <algorithm>

struct Path {
	static Path decompose(std::string string) {
		auto it = string.begin();

		// paths starting with '/' are absolute. 
		bool relative = true;
		if(it != string.end() && *it == '/') {
			relative = false;
			++it;
		}

		// parse each individual component.
		std::vector<std::string> components;
		while(it != string.end()) {
			auto start = std::exchange(it, std::find(it, string.end(), '/'));

			auto component = string.substr(start - string.begin(), it - start);
			if(component == "..") {
				// we resolve double-dots unless they are at the beginning of the path.
				if(components.empty() || components.back() == "..") {
					components.push_back("..");
				}else{
					components.pop_back();
				}
			}else if(!component.empty() && component != ".") {
				// we discard multiple slashes and single-dots.
				components.push_back(std::move(component));
			}

			// finally we need to skip the slash we found.
			if(it != string.end())
				++it;
		}

		return Path(relative, std::move(components));
	}

	using Iterator = std::vector<std::string>::iterator;

	explicit Path(bool relative, std::vector<std::string> components)
	: _relative(relative), _components(std::move(components)) { }

	bool isRelative() {
		return _relative;
	}

	bool empty() {
		return _components.empty();
	}

	Iterator begin() {
		return _components.begin();
	}
	Iterator end() {
		return _components.end();
	}

private:
	bool _relative;
	std::vector<std::string> _components;
};

namespace {

COFIBER_ROUTINE(FutureMaybe<ViewPath>, resolveChild(ViewPath parent, std::string name), ([=] {
	auto child = COFIBER_AWAIT parent.second->getTarget()->getLink(std::move(name));
	if(!child)
		COFIBER_RETURN((ViewPath{parent.first, nullptr})); // TODO: Return an error code.

	auto mount = parent.first->getMount(child);
	if(mount) {
		if(debugResolve)
			std::cout << "    It's a mount point" << std::endl;
		auto origin = mount->getOrigin();
		COFIBER_RETURN((ViewPath{std::move(mount), std::move(origin)}));
	}else{
		if(debugResolve)
			std::cout << "    It's NOT a mount point" << std::endl;
		COFIBER_RETURN((ViewPath{std::move(parent.first), std::move(child)}));
	}
}))

} // anonymous namespace

ViewPath rootPath() {
	return ViewPath{rootView, rootView->getOrigin()};
}

COFIBER_ROUTINE(FutureMaybe<ViewPath>, resolve(ViewPath root, std::string name), ([=] {
	auto path = Path::decompose(std::move(name));

	ViewPath current = root;
	std::deque<std::string> components(path.begin(), path.end());

	while(!components.empty()) {
		auto name = components.front();
		components.pop_front();
		if(debugResolve)
			std::cout << "Resolving " << name << std::endl;

		auto child = COFIBER_AWAIT(resolveChild(current, std::move(name)));
		if(!child.second)
			COFIBER_RETURN(child); // TODO: Return an error code.

		if(child.second->getTarget()->getType() == VfsType::symlink) {
			auto link = Path::decompose(COFIBER_AWAIT child.second->getTarget()->readSymlink());
			if(!link.isRelative())
				current = root;
			components.insert(components.begin(), link.begin(), link.end());
		}else{
			current = std::move(child);
		}
	}

	COFIBER_RETURN(std::move(current));
}))

COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>, open(ViewPath root, std::string name), ([=] {
	ViewPath current = COFIBER_AWAIT resolve(root, std::move(name));
	if(!current.second)
		COFIBER_RETURN(nullptr); // TODO: Return an error code.

	if(current.second->getTarget()->getType() == VfsType::regular) {
		auto file = COFIBER_AWAIT current.second->getTarget()->open(current.second);
		COFIBER_RETURN(std::move(file));
	}else{
		assert(current.second->getTarget()->getType() == VfsType::charDevice);
		auto id = current.second->getTarget()->readDevice();
		auto device = deviceManager.get(id);
		COFIBER_RETURN(COFIBER_AWAIT device->open(current.second));
	}
}))

