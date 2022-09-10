
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <coroutine>
#include <future>

#include "common.hpp"
#include "fs.bragi.hpp"
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

async::result<void> MountView::mount(std::shared_ptr<FsLink> anchor, std::shared_ptr<FsLink> origin) {
	if (anchor) {
		auto result = co_await anchor->obstruct();
		(void)result;
		// result is intentionally ignored to supress warnings
	}

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
	co_await rootView->mount(std::move(dev), getDevtmpfs());

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
			if (component.size())
				components.push_back(std::move(component));

			// finally we need to skip the slash we found.
			if(it != string.end())
				++it;
		}

		return Path{relative, std::move(components), !string.empty() && string.back() == '/'};
	}

	using Iterator = std::vector<std::string>::iterator;

	explicit Path(bool relative, std::vector<std::string> components, bool trailingSlash)
	: _relative(relative), _trailingSlash{trailingSlash}, _components(std::move(components)) { }

	bool isRelative() {
		return _relative;
	}

	bool trailingSlash() {
		return _trailingSlash;
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
	bool _trailingSlash;
	std::vector<std::string> _components;
};

void PathResolver::setup(ViewPath root, ViewPath workdir, std::string string, Process *process) {
	_rootPath = std::move(root);
	_process = process;

	auto path = Path::decompose(std::move(string));
	_components = std::deque<std::string>(path.begin(), path.end());
	_trailingSlash = path.trailingSlash();
	if(path.isRelative()) {
		_currentPath = std::move(workdir);
	}else{
		_currentPath = _rootPath;
	}
}

async::result<frg::expected<protocols::fs::Error, void>> PathResolver::resolve(ResolveFlags flags) {
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

	if((flags & resolveNoTrailingSlash) && _trailingSlash)
		co_return protocols::fs::Error::isDirectory;

	while(true) {
		if(_components.empty()
				|| ((flags & resolvePrefix) && _components.size() == 1))
			break;

		auto name = _components.front();
		_components.pop_front();
		if(debugResolve)
			std::cout << "posix " << sn << ":     Resolving '" << name << "'" << std::endl;

		// Resolve the link into the directory.
		assert(!name.empty()); // This is ensured by the path decomposition algorithm.
		if(name == ".") {
			// Ignore the component.
		}else if(name == "..") {
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
			if (_currentPath.second->getTarget()->hasTraverseLinks()) {
				_components.push_front(name);
				std::string end;

				if (flags & resolvePrefix) {
					end = _components.back();
					_components.pop_back();
				}

				auto result = co_await _currentPath.second->getTarget()->traverseLinks(_components);

				if (!result) {
					assert(result.error() == Error::illegalOperationTarget
							|| result.error() == Error::noSuchFile
							|| result.error() == Error::notDirectory);
					_currentPath = ViewPath{_currentPath.first, nullptr};
					if(result.error() == Error::illegalOperationTarget) {
						std::cout << "\e[33mposix: Illegal operation target in PathResolver::resolve\e[39m" << std::endl;
						co_return protocols::fs::Error::fileNotFound;
					} else if(result.error() == Error::noSuchFile) {
						co_return protocols::fs::Error::fileNotFound;
					} else if(result.error() == Error::notDirectory) {
						co_return protocols::fs::Error::notDirectory;
					}
				}

				auto [child, nLinks] = result.value();

				if (flags & resolvePrefix) {
					_components.push_back(end);
				}

				assert(nLinks <= _components.size());

				while (nLinks--)
					_components.pop_front();

				if(!child) {
					_currentPath = ViewPath{_currentPath.first, nullptr};
					co_return protocols::fs::Error::fileNotFound;
				}

				// Next, we might need to traverse mount boundaries.
				ViewPath next;
				if(auto mount = _currentPath.first->getMount(child); mount) {
					if(debugResolve)
						std::cout << "posix " << sn << ":     VFS path is a mount point" << std::endl;
					next = ViewPath{mount, mount->getOrigin()};
				}else{
					next = ViewPath{_currentPath.first, std::move(child)};
				}

				// Finally, we might need to follow symlinks.
				if(next.second->getTarget()->getType() == VfsType::symlink
						&& !(_components.empty() && (flags & resolveDontFollow))) {
					auto result = co_await next.second->getTarget()->readSymlink(next.second.get(), _process);
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
					else
						_currentPath = ViewPath{_currentPath.first, next.second->getOwner()->treeLink()};
					_components.insert(_components.begin(), link.begin(), link.end());
				}else{
					_currentPath = std::move(next);
				}
			} else {
				auto childResult = co_await _currentPath.second->getTarget()->getLink(std::move(name));
				if(!childResult) {
					assert(childResult.error() == Error::notDirectory
							|| childResult.error() == Error::illegalOperationTarget);
					_currentPath = ViewPath{_currentPath.first, nullptr};
					if(childResult.error() == Error::notDirectory) {
						co_return protocols::fs::Error::notDirectory;
					} else if(childResult.error() == Error::illegalOperationTarget) {
						std::cout << "\e[33mposix: Illegal operation target in PathResolver::resolve\e[39m" << std::endl;
						co_return protocols::fs::Error::fileNotFound;
					}
				}
				auto child = childResult.value();

				if(!child) {
					_currentPath = ViewPath{_currentPath.first, nullptr};
					co_return protocols::fs::Error::fileNotFound;
				}

				// Next, we might need to traverse mount boundaries.
				ViewPath next;
				if(auto mount = _currentPath.first->getMount(child); mount) {
					if(debugResolve)
						std::cout << "posix " << sn << ":     VFS path is a mount point" << std::endl;
					next = ViewPath{mount, mount->getOrigin()};
				}else{
					next = ViewPath{_currentPath.first, std::move(child)};
				}

				// Finally, we might need to follow symlinks.
				if(next.second->getTarget()->getType() == VfsType::symlink
						&& !(_components.empty() && (flags & resolveDontFollow))) {
					auto result = co_await next.second->getTarget()->readSymlink(next.second.get(), _process);
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

	if(flags & resolvePrefix) {
		// If we are resolving a prefix, an empty path is not a valid input.
		if(!_components.size())
			co_return protocols::fs::Error::fileNotFound;
		assert(_components.size() == 1);
	}else{
		assert(!_components.size());

		// If the syntax of the path implies that the path refers to a directory
		// (with a trailing slash), we fail if the node is not actually a directory.
		if(_trailingSlash && _currentPath.second->getTarget()->getType() != VfsType::directory)
			co_return protocols::fs::Error::notDirectory;
	}

	co_return {};
}

std::string ViewPath::getPath(ViewPath root) const {
	std::string path = "/";
	auto dir = *this;
	while(true) {
		if(dir == root)
			break;

		// If we are at the origin of a mount point, traverse that mount point.
		ViewPath traversed;
		if(dir.second == dir.first->getOrigin()) {
			if(!dir.first->getParent())
				break;
			auto anchor = dir.first->getAnchor();
			assert(anchor); // Non-root mounts must have anchors in their parents.
			traversed = ViewPath{dir.first->getParent(), anchor};
		}else{
			traversed = dir;
		}

		auto owner = traversed.second->getOwner();
		assert(owner); // Otherwise, we would have been at the root.
		path = "/" + traversed.second->getName() + path;

		dir = ViewPath{traversed.first, owner->treeLink()};
	}

	return path;
}

ViewPath rootPath() {
	return ViewPath{rootView, rootView->getOrigin()};
}

async::result<frg::expected<protocols::fs::Error, ViewPath>> resolve(ViewPath root, ViewPath workdir,
		std::string name, Process *process, ResolveFlags flags) {
	PathResolver resolver;
	resolver.setup(std::move(root), std::move(workdir), std::move(name), process);
	auto result = co_await resolver.resolve(flags);
	if (!result) {
		assert(result.error() == protocols::fs::Error::fileNotFound
				|| result.error() == protocols::fs::Error::notDirectory);
		if(result.error() == protocols::fs::Error::fileNotFound) {
			co_return protocols::fs::Error::fileNotFound;
		} else if(result.error() == protocols::fs::Error::notDirectory) {
			co_return protocols::fs::Error::notDirectory;
		}
	}
	co_return ViewPath(resolver.currentView(), resolver.currentLink());
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> open(ViewPath root,
		ViewPath workdir, std::string name, Process *process, ResolveFlags resolve_flags,
		SemanticFlags semantic_flags) {
	auto resolveResult = co_await resolve(std::move(root), std::move(workdir),
			std::move(name), process, resolve_flags);
	if (!resolveResult) {
		assert(resolveResult.error() == protocols::fs::Error::fileNotFound
				|| resolveResult.error() == protocols::fs::Error::notDirectory);
		if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
			co_return Error::noSuchFile;
		} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
			co_return Error::notDirectory;
		}
	}
	ViewPath current = resolveResult.value();

	auto file = co_await current.second->getTarget()->open(current.first, current.second,
			semantic_flags);
	co_return std::move(file);
}

