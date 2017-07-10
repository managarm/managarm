
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
// File implementation.
// --------------------------------------------------------

COFIBER_ROUTINE(FutureMaybe<void>, readExactly(std::shared_ptr<File> file,
		void *data, size_t length), ([=] {
	size_t offset = 0;
	while(offset < length) {
		auto result = COFIBER_AWAIT readSome(file, (char *)data + offset, length - offset);
		assert(result > 0);
		offset += result;
	}

	COFIBER_RETURN();
}))

FutureMaybe<off_t> seek(std::shared_ptr<File> file, off_t offset, VfsSeek whence) {
	assert(file);
	assert(file->operations()->seek);
	return file->operations()->seek(file, offset, whence);
}

FutureMaybe<size_t> readSome(std::shared_ptr<File> file, void *data, size_t max_length) {
	assert(file);
	assert(file->operations()->readSome);
	return file->operations()->readSome(file, data, max_length);
}

FutureMaybe<helix::UniqueDescriptor> accessMemory(std::shared_ptr<File> file) {
	assert(file);
	assert(file->operations()->accessMemory);
	return file->operations()->accessMemory(file);
}

helix::BorrowedDescriptor getPassthroughLane(std::shared_ptr<File> file) {
	assert(file);
	assert(file->operations()->getPassthroughLane);
	return file->operations()->getPassthroughLane(file);
}

// --------------------------------------------------------
// Link implementation.
// --------------------------------------------------------

namespace {
	struct RootLink : Link {
	private:
		static std::shared_ptr<Node> getOwner(std::shared_ptr<Link> object) {
			(void)object;
			return std::shared_ptr<Node>{};
		}

		static std::string getName(std::shared_ptr<Link> object) {
			(void)object;
			assert(!"No associated name");
		}

		static std::shared_ptr<Node> getTarget(std::shared_ptr<Link> object) {
			auto derived = std::static_pointer_cast<RootLink>(object);
			return derived->_target;
		}

		static const LinkOperations operations;

	public:
		RootLink(std::shared_ptr<Node> target)
		: Link(&operations), _target{std::move(target)} { }

	private:
		std::shared_ptr<Node> _target;
	};

	const LinkOperations RootLink::operations{
		&RootLink::getOwner,
		&RootLink::getName,
		&RootLink::getTarget
	};
}

std::shared_ptr<Link> createRootLink(std::shared_ptr<Node> target) {
	return std::make_shared<RootLink>(std::move(target));
}

std::shared_ptr<Node> getTarget(std::shared_ptr<Link> link) {
	assert(link);
	assert(link->operations()->getTarget);
	return link->operations()->getTarget(link);
}

// --------------------------------------------------------
// Node implementation.
// --------------------------------------------------------

VfsType getType(std::shared_ptr<Node> node) {
	assert(node);
	return node->operations()->getType(node);
}

FileStats getStats(std::shared_ptr<Node> node) {
	assert(node);
	assert(node->operations()->getStats);
	return node->operations()->getStats(node);
}

FutureMaybe<std::shared_ptr<Link>> getLink(std::shared_ptr<Node> node, std::string name) {
	assert(node);
	assert(node->operations()->getLink);
	return node->operations()->getLink(node, std::move(name));
}

FutureMaybe<std::shared_ptr<Link>> link(std::shared_ptr<Node> node, std::string name,
		std::shared_ptr<Node> target) {
	assert(node);
	assert(node->operations()->link);
	return node->operations()->link(node, std::move(name), std::move(target));
}

FutureMaybe<std::shared_ptr<Link>> mkdir(std::shared_ptr<Node> node, std::string name) {
	assert(node);
	assert(node->operations()->mkdir);
	return node->operations()->mkdir(node, std::move(name));
}

FutureMaybe<std::shared_ptr<Link>> symlink(std::shared_ptr<Node> node,
		std::string name, std::string path) {
	assert(node);
	assert(node->operations()->symlink);
	return node->operations()->symlink(node, std::move(name), std::move(path));
}

FutureMaybe<std::shared_ptr<Link>> mkdev(std::shared_ptr<Node> node,
		std::string name, VfsType type, DeviceId id) {
	assert(node);
	assert(node->operations()->mkdev);
	return node->operations()->mkdev(node, std::move(name), type, id);
}

FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<Node> node) {
	assert(node);
	assert(node->operations()->open);
	return node->operations()->open(node);
}

FutureMaybe<std::string> readSymlink(std::shared_ptr<Node> node) {
	assert(node);
	assert(node->operations()->readSymlink);
	return node->operations()->readSymlink(node);
}

DeviceId readDevice(std::shared_ptr<Node> node) {
	assert(node);
	assert(node->operations()->readDevice);
	return node->operations()->readDevice(node);
}
// --------------------------------------------------------
// MountView implementation.
// --------------------------------------------------------

std::shared_ptr<MountView> MountView::createRoot(std::shared_ptr<Link> origin) {
	return std::make_shared<MountView>(nullptr, nullptr, std::move(origin));
}

std::shared_ptr<Link> MountView::getAnchor() const {
	return _anchor;
}

std::shared_ptr<Link> MountView::getOrigin() const {
	return _origin;
}

void MountView::mount(std::shared_ptr<Link> anchor, std::shared_ptr<Link> origin) {
	_mounts.insert(std::make_shared<MountView>(shared_from_this(),
			std::move(anchor), std::move(origin)));
	// TODO: check insert return value
}

std::shared_ptr<MountView> MountView::getMount(std::shared_ptr<Link> link) const {
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

	COFIBER_AWAIT mkdir(getTarget(tree), "realfs");
	
	auto dev = COFIBER_AWAIT mkdir(getTarget(tree), "dev");
	rootView->mount(std::move(dev), getDevtmpfs());

	// Populate the tmpfs from the fs we are running on.
	std::vector<
		std::pair<
			std::shared_ptr<Node>,
			std::string
		>
	> stack;

	stack.push_back({getTarget(tree), std::string{""}});
	
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

//			std::cout << "posix: Importing " << item.second + "/" + resp.path() << std::endl;

			if(resp.file_type() == managarm::fs::FileType::DIRECTORY) {
				auto link = COFIBER_AWAIT mkdir(item.first, resp.path());
				stack.push_back({getTarget(link), item.second + "/" + resp.path()});
			}else{
				assert(resp.file_type() == managarm::fs::FileType::REGULAR);

				auto file_path = "/" + item.second + "/" + resp.path();
				auto node = tmp_fs::createMemoryNode(std::move(file_path));
				COFIBER_AWAIT link(item.first, resp.path(), node);
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
	auto child = COFIBER_AWAIT getLink(getTarget(parent.second), std::move(name));
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

		if(getType(getTarget(child.second)) == VfsType::symlink) {
			auto link = Path::decompose(COFIBER_AWAIT readSymlink(getTarget(child.second)));
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

	if(getType(getTarget(current.second)) == VfsType::regular) {
		auto file = COFIBER_AWAIT open(getTarget(current.second));
		COFIBER_RETURN(std::move(file));
	}else{
		assert(getType(getTarget(current.second)) == VfsType::charDevice);
		auto id = readDevice(getTarget(current.second));
		auto device = deviceManager.get(id);
		COFIBER_RETURN(COFIBER_AWAIT open(device, getTarget(current.second)));
	}
}))

