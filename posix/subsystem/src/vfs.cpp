
#include <string.h>
#include <future>

#include <cofiber.hpp>
#include <cofiber/future.hpp>

#include "common.hpp"
#include "vfs.hpp"
#include "device.hpp"
#include "tmp_fs.hpp"
#include "extern_fs.hpp"

static bool debugResolve = false;

// --------------------------------------------------------
// File implementation.
// --------------------------------------------------------

COFIBER_ROUTINE(FutureMaybe<void>, readExactly(std::shared_ptr<File> file, void *data, size_t length),
		([=] {
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
	return file->operations()->seek(file, offset, whence);
}

FutureMaybe<size_t> readSome(std::shared_ptr<File> file, void *data, size_t max_length) {
	assert(file);
	return file->operations()->readSome(file, data, max_length);
}

FutureMaybe<helix::UniqueDescriptor> accessMemory(std::shared_ptr<File> file) {
	assert(file);
	return file->operations()->accessMemory(file);
}

helix::BorrowedDescriptor getPassthroughLane(std::shared_ptr<File> file) {
	assert(file);
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
	return node->operations()->getStats(node);
}

FutureMaybe<std::shared_ptr<Link>> getLink(std::shared_ptr<Node> node, std::string name) {
	assert(node);
	return node->operations()->getLink(node, std::move(name));
}

FutureMaybe<std::shared_ptr<Link>> mkdir(std::shared_ptr<Node> node, std::string name) {
	assert(node);
	return node->operations()->mkdir(node, std::move(name));
}

FutureMaybe<std::shared_ptr<Link>> symlink(std::shared_ptr<Node> node,
		std::string name, std::string path) {
	assert(node);
	return node->operations()->symlink(node, std::move(name), std::move(path));
}

FutureMaybe<std::shared_ptr<Link>> mkdev(std::shared_ptr<Node> node,
		std::string name, VfsType type, DeviceId id) {
	assert(node);
	return node->operations()->mkdev(node, std::move(name), type, id);
}

FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<Node> node) {
	assert(node);
	return node->operations()->open(node);
}

FutureMaybe<std::string> readSymlink(std::shared_ptr<Node> node) {
	assert(node);
	return node->operations()->readSymlink(node);
}

DeviceId readDevice(std::shared_ptr<Node> node) {
	assert(node);
	return node->operations()->readDevice(node);
}
// --------------------------------------------------------
// SharedView implementation.
// --------------------------------------------------------

SharedView SharedView::createRoot(std::shared_ptr<Link> origin) {
	auto data = std::make_shared<Data>();
	data->origin = std::move(origin);
	return SharedView{std:move(data)};
}

std::shared_ptr<Link> SharedView::getAnchor() const {
	return _data->anchor;
}

std::shared_ptr<Link> SharedView::getOrigin() const {
	return _data->origin;
}

void SharedView::mount(std::shared_ptr<Link> anchor, std::shared_ptr<Link> origin) const {
	auto data = std::make_shared<Data>();
	data->anchor = std::move(anchor);
	data->origin = std::move(origin);
	_data->mounts.insert(SharedView{std::move(data)});
	// TODO: check insert return value
}

SharedView SharedView::getMount(std::shared_ptr<Link> link) const {
	auto it = _data->mounts.find(link);
	if(it == _data->mounts.end())
		return SharedView{};
	return *it;
}

namespace {

COFIBER_ROUTINE(std::future<SharedView>, createRootView(), ([=] {
	auto tree = tmp_fs::createRoot();
	auto view = SharedView::createRoot(tree);

	COFIBER_AWAIT mkdir(getTarget(tree), "realfs");
	
	auto lib = COFIBER_AWAIT mkdir(getTarget(tree), "lib");

	// create a /dev directory + device files.
	auto dev = COFIBER_AWAIT mkdir(getTarget(tree), "dev");
	view.mount(std::move(dev), getDevtmpfs());

	// mount the initrd to /initrd.
	auto initrd = COFIBER_AWAIT mkdir(getTarget(tree), "initrd");
	view.mount(std::move(initrd), extern_fs::createRoot());

	// symlink files from / to /initrd.
	COFIBER_AWAIT symlink(getTarget(lib), "ld-init.so", "/initrd/ld-init.so");
	COFIBER_AWAIT symlink(getTarget(tree), "posix-init", "initrd/posix-init");
	COFIBER_AWAIT symlink(getTarget(tree), "uhci", "initrd/uhci");
	COFIBER_AWAIT symlink(getTarget(tree), "libc.so", "initrd/libc.so");
	COFIBER_AWAIT symlink(getTarget(tree), "libm.so", "initrd/libm.so");
	COFIBER_AWAIT symlink(getTarget(tree), "libgcc_s.so.1", "initrd/libgcc_s.so.1");
	COFIBER_AWAIT symlink(getTarget(tree), "libstdc++.so.6", "initrd/libstdc++.so.6");
	COFIBER_AWAIT symlink(getTarget(tree), "libarch.so", "initrd/libarch.so");
	COFIBER_AWAIT symlink(getTarget(tree), "libhelix.so", "initrd/libhelix.so");
	COFIBER_AWAIT symlink(getTarget(tree), "libcofiber.so", "initrd/libcofiber.so");
	COFIBER_AWAIT symlink(getTarget(tree), "libprotobuf-lite.so.11", "initrd/libprotobuf-lite.so.11");
	COFIBER_AWAIT symlink(getTarget(tree), "libfs_protocol.so", "initrd/libfs_protocol.so");
	COFIBER_AWAIT symlink(getTarget(tree), "libhw_protocol.so", "initrd/libhw_protocol.so");
	COFIBER_AWAIT symlink(getTarget(tree), "libmbus_protocol.so", "initrd/libmbus_protocol.so");
	COFIBER_AWAIT symlink(getTarget(tree), "libusb_protocol.so", "initrd/libusb_protocol.so");
	COFIBER_AWAIT symlink(getTarget(tree), "libblockfs.so", "initrd/libblockfs.so");
	COFIBER_AWAIT symlink(getTarget(tree), "libevbackend.so", "initrd/libevbackend.so");

	COFIBER_RETURN(std::move(view));
}))

SharedView rootView = createRootView().get();

} // anonymous namespace

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

	auto mount = parent.first.getMount(child);
	if(mount) {
		if(debugResolve)
			std::cout << "    It's a mount point" << std::endl;
		auto origin = mount.getOrigin();
		COFIBER_RETURN((ViewPath{std::move(mount), std::move(origin)}));
	}else{
		if(debugResolve)
			std::cout << "    It's NOT a mount point" << std::endl;
		COFIBER_RETURN((ViewPath{std::move(parent.first), std::move(child)}));
	}
}))

} // anonymous namespace

ViewPath rootPath() {
	return ViewPath{rootView, rootView.getOrigin()};
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

