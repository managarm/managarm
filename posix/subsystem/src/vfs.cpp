
#include <string.h>
#include <future>

#include <cofiber.hpp>
#include <cofiber/future.hpp>

#include "common.hpp"
#include "vfs.hpp"
#include "tmp_fs.hpp"
#include "extern_fs.hpp"

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
	return file->operations()->seek(file, offset, whence);
}

FutureMaybe<size_t> readSome(std::shared_ptr<File> file, void *data, size_t max_length) {
	return file->operations()->readSome(file, data, max_length);
}

FutureMaybe<helix::UniqueDescriptor> accessMemory(std::shared_ptr<File> file) {
	return file->operations()->accessMemory(file);
}

helix::BorrowedDescriptor getPassthroughLane(std::shared_ptr<File> file) {
	return file->operations()->getPassthroughLane(file);
}

// --------------------------------------------------------
// SharedLink implementation.
// --------------------------------------------------------

SharedLink SharedLink::createRoot(SharedNode target) {
	struct Data : LinkData {
		Data(SharedNode target)
		: _target{std::move(target)} { }

		SharedNode getOwner() override {
			return SharedNode{};
		}

		std::string getName() override {
			assert(!"No associated name");
		}

		SharedNode getTarget() override {
			return _target;
		}

	private:
		SharedNode _target;
	};

	return SharedLink{std::make_shared<Data>(std::move(target))};
}

bool SharedLink::operator< (const SharedLink &other) const {
	return _data < other._data;
}

SharedNode SharedLink::getTarget() const {
	return _data->getTarget();
}

// --------------------------------------------------------
// SharedNode implementation.
// --------------------------------------------------------

bool SharedNode::operator< (const SharedNode &other) const {
	return _data < other._data;
}

VfsType SharedNode::getType() const {
	return _data->getType();
}

FutureMaybe<SharedLink> SharedNode::getLink(std::string name) const {
	return std::static_pointer_cast<TreeData>(_data)->getLink(std::move(name));
}

FutureMaybe<SharedLink> SharedNode::mkdir(std::string name) const {
	return std::static_pointer_cast<TreeData>(_data)->mkdir(std::move(name));
}

FutureMaybe<SharedLink> SharedNode::symlink(std::string name, std::string path) const {
	return std::static_pointer_cast<TreeData>(_data)->symlink(std::move(name), std::move(path));
}

FutureMaybe<std::shared_ptr<File>> SharedNode::open() const {
	return std::static_pointer_cast<RegularData>(_data)->open();
}

FutureMaybe<std::string> SharedNode::readSymlink() const {
	return std::static_pointer_cast<SymlinkData>(_data)->readSymlink();
}

// --------------------------------------------------------
// SharedView implementation.
// --------------------------------------------------------

SharedView SharedView::createRoot(SharedLink origin) {
	auto data = std::make_shared<Data>();
	data->origin = std::move(origin);
	return SharedView{std:move(data)};
}

SharedLink SharedView::getAnchor() const {
	return _data->anchor;
}

SharedLink SharedView::getOrigin() const {
	return _data->origin;
}

void SharedView::mount(SharedLink anchor, SharedLink origin) const {
	auto data = std::make_shared<Data>();
	data->anchor = std::move(anchor);
	data->origin = std::move(origin);
	_data->mounts.insert(SharedView{std::move(data)});
	// TODO: check insert return value
}

SharedView SharedView::getMount(SharedLink link) const {
	auto it = _data->mounts.find(link);
	if(it == _data->mounts.end())
		return SharedView{};
	return *it;
}

namespace {

COFIBER_ROUTINE(std::future<SharedView>, createRootView(), ([=] {
	auto tree = tmp_fs::createRoot();
	auto view = SharedView::createRoot(tree);

	// mount the initrd to /initrd.
	auto link = COFIBER_AWAIT(tree.getTarget().mkdir("initrd"));
	view.mount(std::move(link), extern_fs::createRoot());

	// symlink files from / to /initrd.
	COFIBER_AWAIT(tree.getTarget().symlink("ld-init.so", "initrd/ld-init.so"));
	COFIBER_AWAIT(tree.getTarget().symlink("posix-init", "initrd/posix-init"));
	COFIBER_AWAIT(tree.getTarget().symlink("libgcc_s.so.1", "initrd/libgcc_s.so.1"));
	COFIBER_AWAIT(tree.getTarget().symlink("libstdc++.so.6", "initrd/libstdc++.so.6"));
	COFIBER_AWAIT(tree.getTarget().symlink("libc.so", "initrd/libc.so"));
	COFIBER_AWAIT(tree.getTarget().symlink("libm.so", "initrd/libm.so"));

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

using ViewPath = std::pair<SharedView, SharedLink>;

COFIBER_ROUTINE(FutureMaybe<ViewPath>, resolveChild(ViewPath parent, std::string name), ([=] {
	auto child = COFIBER_AWAIT parent.second.getTarget().getLink(std::move(name));
	auto mount = parent.first.getMount(child);
	if(mount) {
//		std::cout << "It's a mount point" << std::endl;
		auto origin = mount.getOrigin();
		COFIBER_RETURN((ViewPath{std::move(mount), std::move(origin)}));
	}else{
//		std::cout << "It's NOT a mount point" << std::endl;
		COFIBER_RETURN((ViewPath{std::move(parent.first), std::move(child)}));
	}
}))

} // anonymous namespace

COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>, open(std::string name), ([=] {
	auto path = Path::decompose(std::move(name));
	std::deque<std::string> components(path.begin(), path.end());

	ViewPath current{rootView, rootView.getOrigin()};

	while(!components.empty()) {
		auto name = components.front();
		components.pop_front();
		std::cout << "Resolving " << name << std::endl;

		auto child = COFIBER_AWAIT(resolveChild(current, std::move(name)));
		if(child.second.getTarget().getType() == VfsType::symlink) {
			auto link = Path::decompose(COFIBER_AWAIT child.second.getTarget().readSymlink());
			components.insert(components.begin(), link.begin(), link.end());
		}else{
			current = std::move(child);
		}
	}

//	std::cout << "Opening file..." << std::endl;

	assert(current.second.getTarget().getType() == VfsType::regular);
	auto file = COFIBER_AWAIT current.second.getTarget().open();
	COFIBER_RETURN(std::move(file));
}))

