#pragma once

#include <string.h>
#include <iostream>
#include <set>
#include <deque>

#include <async/result.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <hel.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "file.hpp"
#include "fs.hpp"

using ResolveFlags = uint32_t;
inline constexpr ResolveFlags resolvePrefix = (1 << 4);
// The path must not refer to a directory (not trailing slash allowed).
inline constexpr ResolveFlags resolveNoTrailingSlash = (1 << 2);
inline constexpr ResolveFlags resolveDontFollow = (1 << 1);

using ViewPathPair = std::pair<std::shared_ptr<MountView>, std::shared_ptr<FsLink>>;

struct ViewPath : public ViewPathPair {
	ViewPath() = default;

	ViewPath(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: ViewPathPair(mount, link) {}

	std::string getPath(ViewPath root) const;
};

//! Represents a virtual view of the file system.
//! We handle all mount point related logic in this class.
struct MountView : std::enable_shared_from_this<MountView> {
	static std::shared_ptr<MountView> createRoot(std::shared_ptr<FsLink> origin);

	// TODO: This is an implementation detail that could be hidden.
	explicit MountView(uint64_t mountId, std::shared_ptr<MountView> parent, std::shared_ptr<FsLink> anchor,
			std::shared_ptr<FsLink> origin, ViewPath deviceLink)
	: mountId_{mountId}, _parent{std::move(parent)}, _anchor{std::move(anchor)}, _origin{std::move(origin)},
		deviceLink_{std::move(deviceLink)}
	{ }

	uint64_t mountId() const {
		return mountId_;
	}

	std::shared_ptr<MountView> getParent() const;
	std::shared_ptr<FsLink> getAnchor() const;
	std::shared_ptr<FsLink> getOrigin() const;
	ViewPath getDevice() const {
		return deviceLink_;
	}

	async::result<void> mount(std::shared_ptr<FsLink> anchor, std::shared_ptr<FsLink> origin, ViewPath deviceLink = {});
	async::result<void> unmount(std::shared_ptr<MountView> view);

	std::shared_ptr<MountView> getMount(std::shared_ptr<FsLink> link) const;

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

	const std::set<std::shared_ptr<MountView>, Compare> &mounts() const {
		return _mounts;
	}

private:

	uint64_t mountId_;
	std::shared_ptr<MountView> _parent;
	std::shared_ptr<FsLink> _anchor;
	std::shared_ptr<FsLink> _origin;
	ViewPath deviceLink_;
	std::set<std::shared_ptr<MountView>, Compare> _mounts;
};

struct PathResolver {
	void setup(ViewPath root, ViewPath workdir, std::string string, Process *process);

	async::result<frg::expected<protocols::fs::Error, void>> resolve(ResolveFlags flags = 0);

	bool hasComponent() {
		return !_components.empty();
	}

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
	Process *_process;

	std::deque<std::string> _components;
	bool _trailingSlash;
	ViewPath _currentPath;
};

async::result<void> populateRootView();

ViewPath rootPath();

// TODO: Switch to PathResolver instead of using this function.
async::result<frg::expected<protocols::fs::Error, ViewPath>> resolve(ViewPath root, ViewPath workdir,
		std::string name, Process *process, ResolveFlags flags = 0);

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>> open(ViewPath root,
		ViewPath workdir, std::string name, Process *process, ResolveFlags resolve_flags = 0,
		SemanticFlags semantic_flags = 0);
