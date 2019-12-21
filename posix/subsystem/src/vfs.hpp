#ifndef POSIX_SUBSYSTEM_VFS_HPP
#define POSIX_SUBSYSTEM_VFS_HPP

#include <string.h>
#include <iostream>
#include <set>
#include <deque>

#include <async/result.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <hel.h>

#include "file.hpp"
#include "fs.hpp"

using ResolveFlags = uint32_t;
inline constexpr ResolveFlags resolvePrefix = (1 << 4);
inline constexpr ResolveFlags resolveDontFollow = (1 << 1);

//! Represents a virtual view of the file system.
//! We handle all mount point related logic in this class.
struct MountView : std::enable_shared_from_this<MountView> {
	static std::shared_ptr<MountView> createRoot(std::shared_ptr<FsLink> origin);

	// TODO: This is an implementation detail that could be hidden.
	explicit MountView(std::shared_ptr<MountView> parent, std::shared_ptr<FsLink> anchor,
			std::shared_ptr<FsLink> origin)
	: _parent{std::move(parent)}, _anchor{std::move(anchor)}, _origin{std::move(origin)} { }


	std::shared_ptr<MountView> getParent() const;
	std::shared_ptr<FsLink> getAnchor() const;
	std::shared_ptr<FsLink> getOrigin() const;

	void mount(std::shared_ptr<FsLink> anchor, std::shared_ptr<FsLink> origin);

	std::shared_ptr<MountView> getMount(std::shared_ptr<FsLink> link) const;

private:
	struct Compare {
		struct is_transparent { };

		bool operator() (const std::shared_ptr<MountView> &a,
				const std::shared_ptr<FsLink> &b) const {
			return a->getAnchor() < b;
		}
		bool operator() (const std::shared_ptr<FsLink> &a,
				const std::shared_ptr<MountView> &b) const {
			return a < b->getAnchor();
		}

		bool operator() (const std::shared_ptr<MountView> &a,
				const std::shared_ptr<MountView> &b) const {
			return a->getAnchor() < b->getAnchor();
		}
	};

	std::shared_ptr<MountView> _parent;
	std::shared_ptr<FsLink> _anchor;
	std::shared_ptr<FsLink> _origin;
	std::set<std::shared_ptr<MountView>, Compare> _mounts;
};

using ViewPath = std::pair<std::shared_ptr<MountView>, std::shared_ptr<FsLink>>;

struct PathResolver {
	void setup(ViewPath root, ViewPath workdir, std::string string);

	async::result<void> resolve(ResolveFlags flags = 0);

	std::string nextComponent() {
		assert(!_components.empty());
		return _components.front();
	}
	
	std::shared_ptr<MountView> currentView() {
		return _currentPath.first;
	}

	std::shared_ptr<FsLink> currentLink() {
		return _currentPath.second;
	}

private:
	ViewPath _rootPath;

	std::deque<std::string> _components;
	ViewPath _currentPath;
};

async::result<void> populateRootView();

ViewPath rootPath();

// TODO: Switch to PathResolver instead of using this function.
FutureMaybe<ViewPath> resolve(ViewPath root, ViewPath workdir,
		std::string name, ResolveFlags flags = 0);

FutureMaybe<smarter::shared_ptr<File, FileHandle>> open(ViewPath root, ViewPath workdir,
		std::string name, ResolveFlags resolve_flags = 0, SemanticFlags semantic_flags = 0);

#endif // POSIX_SUBSYSTEM_VFS_HPP
