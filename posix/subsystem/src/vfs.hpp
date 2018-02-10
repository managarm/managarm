#ifndef POSIX_SUBSYSTEM_VFS_HPP
#define POSIX_SUBSYSTEM_VFS_HPP

#include <iostream>
#include <set>

#include <async/result.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>
#include <hel.h>

#include "file.hpp"
#include "fs.hpp"

namespace _vfs_view {
	struct MountView;
};

using _vfs_view::MountView;

namespace _vfs_view {

//! Represents a virtual view of the file system.
//! We handle all mount point related logic in this class.
struct MountView : std::enable_shared_from_this<MountView> {
	static std::shared_ptr<MountView> createRoot(std::shared_ptr<FsLink> origin);

	// TODO: This is an implementation detail that could be hidden.
	explicit MountView(std::shared_ptr<MountView> parent, std::shared_ptr<FsLink> anchor,
			std::shared_ptr<FsLink> origin)
	: _parent{std::move(parent)}, _anchor{std::move(anchor)}, _origin{std::move(origin)} { }

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

} // namespace _vfs_view

using ViewPath = std::pair<std::shared_ptr<MountView>, std::shared_ptr<FsLink>>;

async::result<void> populateRootView();

ViewPath rootPath();

using ResolveFlags = uint32_t;
inline constexpr ResolveFlags resolveDontFollow = (1 << 1);

FutureMaybe<ViewPath> resolve(ViewPath root, std::string name, ResolveFlags flags = 0);

FutureMaybe<std::shared_ptr<File>> open(ViewPath root, std::string name);

#endif // POSIX_SUBSYSTEM_VFS_HPP
