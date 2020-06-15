
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <experimental/coroutine>
#include <future>

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

std::shared_ptr<MountView> MountView::getParent() const {
	return _parent;
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

async::result<void> populateRootView() {
	// Create a tmpfs instance for the initrd.
	auto tree = tmp_fs::createRoot();
	rootView = MountView::createRoot(tree);

	co_await tree->getTarget()->mkdir("realfs");
	
	// TODO: Check for errors from mkdir().
	auto dev = std::get<std::shared_ptr<FsLink>>(co_await tree->getTarget()->mkdir("dev"));
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
			managarm::fs::CntRequest req;
			req.set_req_type(managarm::fs::CntReqType::PT_READ_ENTRIES);

			auto ser = req.SerializeAsString();
			auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::offer(
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::recvInline()
				)
			);
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
				// TODO: Check for errors from mkdir().
				auto link = std::get<std::shared_ptr<FsLink>>(
						co_await item.first->mkdir(resp.path()));
				stack.push_back({link->getTarget(), item.second + "/" + resp.path()});
			}else{
				assert(resp.file_type() == managarm::fs::FileType::REGULAR);

				auto file_path = "/" + item.second + "/" + resp.path();
				auto node = tmp_fs::createMemoryNode(std::move(file_path));
				auto result = co_await item.first->link(resp.path(), node);
				assert(result);
			}
		}
	}
}

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
			// Note: Due to the way .. interacts with symlins, do not resolve it here!
			// However, we discard multiple slashes and single-dots.
			if(!component.empty() && component != ".") {
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

void PathResolver::setup(ViewPath root, ViewPath workdir, std::string string) {
	_rootPath = std::move(root);

	auto path = Path::decompose(std::move(string));
	_components = std::deque<std::string>(path.begin(), path.end());
	if(path.isRelative()) {
		_currentPath = std::move(workdir);
	}else{
		_currentPath = _rootPath;
	}
}

async::result<void> PathResolver::resolve(ResolveFlags flags) {
	auto sn = StructName::get("path-resolve");
	if(debugResolve) {
		std::cout << "posix " << sn << ": Path resolution for '";
		for(auto it = _components.begin(); it != _components.end(); ++it) {
			if(it != _components.begin())
				std::cout << "/";
			std::cout << *it;
		}
		std::cout << "'" << std::endl;
	}

	while(!_components.empty()
			&& (!(flags & resolvePrefix) || _components.size() > 1)) {
		auto name = _components.front();
		_components.pop_front();
		if(debugResolve)
			std::cout << "posix " << sn << ":     Resolving '" << name << "'" << std::endl;

		// Resolve the link into the directory.
		if(name == "..") {
			if(_currentPath == _rootPath) {
				// We are at the root -- do not modify _currentPath at all.
			}else if(_currentPath.second == _currentPath.first->getOrigin()) {
				if(auto parent = _currentPath.first->getParent(); parent) {
					auto anchor = _currentPath.first->getAnchor();
					assert(anchor); // Non-root mounts must have anchors in their parents.
					auto owner = anchor->getOwner();
					// TODO: We have to decide if the following is something we want to support:
					assert(owner && "FS mounted over root of parent mount");
					_currentPath = ViewPath{parent, owner->treeLink()};
				}else{
					// We are at the root -- do not modify _currentPath at all.
				}
			}else{
				auto owner = _currentPath.second->getOwner();
				assert(owner && "VFS resolution crosses root directory of non-root"
							" mount! (Mount configuration broken?)");
				_currentPath = ViewPath{_currentPath.first, owner->treeLink()};
			}
		}else{
			auto childResult = co_await _currentPath.second->getTarget()->getLink(std::move(name));
			assert(childResult);
			auto child = childResult.value();

			if(!child) {
				// TODO: Return an error code.
				_currentPath = ViewPath{_currentPath.first, nullptr};
				co_return;
			}

			// Next, we might need to traverse mount boundaries.
			ViewPath next;
			if(auto mount = _currentPath.first->getMount(child); mount) {
				if(debugResolve)
					std::cout << "posix " << sn << ":     VFS path is a mount point" << std::endl;
				next = ViewPath{std::move(mount), mount->getOrigin()};
			}else{
				next = ViewPath{_currentPath.first, std::move(child)};
			}

			// Finally, we might need to follow symlinks.
			if(next.second->getTarget()->getType() == VfsType::symlink
					&& !(_components.empty() && (flags & resolveDontFollow))) {
				auto result = co_await next.second->getTarget()->readSymlink(next.second.get());
				auto link = Path::decompose(std::get<std::string>(result));

				if(debugResolve) {
					std::cout << "posix " << sn << ":     Link target is a symlink to '"
							<< (link.isRelative() ? "" : "/");
					for(auto it = link.begin(); it != link.end(); ++it) {
						if(it != link.begin())
							std::cout << "/";
						std::cout << *it;
					}
					std::cout << "'" << std::endl;
				}

				if(!link.isRelative())
					_currentPath = _rootPath;
				_components.insert(_components.begin(), link.begin(), link.end());
			}else{
				_currentPath = std::move(next);
			}
		}
	}
}

ViewPath rootPath() {
	return ViewPath{rootView, rootView->getOrigin()};
}

FutureMaybe<ViewPath> resolve(ViewPath root, ViewPath workdir,
		std::string name, ResolveFlags flags) {
	PathResolver resolver;
	resolver.setup(std::move(root), std::move(workdir), std::move(name));
	co_await resolver.resolve(flags);
	co_return ViewPath(resolver.currentView(), resolver.currentLink());
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> open(ViewPath root,
		ViewPath workdir, std::string name, ResolveFlags resolve_flags,
		SemanticFlags semantic_flags) {
	ViewPath current = co_await resolve(std::move(root), std::move(workdir),
			std::move(name), resolve_flags);
	if(!current.second)
		co_return nullptr; // TODO: Return an error code.

	auto file = co_await current.second->getTarget()->open(current.first, current.second,
			semantic_flags);
	co_return std::move(file);
}

