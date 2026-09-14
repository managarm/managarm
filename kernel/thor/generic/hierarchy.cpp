#include <frg/manual_box.hpp>

#include <thor-internal/debug.hpp>
#include <thor-internal/hierarchy.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/rcu.hpp>

namespace thor {

namespace {
	std::atomic<uint64_t> nextHierarchyId{1};
}

Hierarchy::Hierarchy(CtorToken, smarter::shared_ptr<Hierarchy> parent, frg::string<KernelAlloc> tag)
: id_{nextHierarchyId.fetch_add(1, std::memory_order_relaxed)},
		parent_{std::move(parent)}, tag_{std::move(tag)} { }

void Hierarchy::finalizeBeforeRcu() {
	if(!parent_)
		return;

	auto lock = frg::guard(&parent_->childrenMutex_);
	parent_->children_.erase(this);
}

std::expected<smarter::shared_ptr<Hierarchy>, Error> Hierarchy::createRoot() {
	auto ptr = allocate_rcu_shared<Hierarchy>(
		*kernelAlloc,
		CtorToken{},
		smarter::shared_ptr<Hierarchy>{},
		frg::string<KernelAlloc>{*kernelAlloc, "root"}
	);
	if(!ptr)
		return std::unexpected{Error::noMemory};
	ptr->selfPtr_ = ptr;
	return ptr;
}

std::expected<smarter::shared_ptr<Hierarchy>, Error> Hierarchy::extend(
	smarter::shared_ptr<Hierarchy> parent, frg::string<KernelAlloc> tag
) {
	if(!parent)
		return std::unexpected{Error::illegalArgs};
	auto ptr = allocate_rcu_shared<Hierarchy>(
		*kernelAlloc, CtorToken{}, std::move(parent), std::move(tag)
	);
	if(!ptr)
		return std::unexpected{Error::noMemory};
	ptr->selfPtr_ = ptr;

	{
		auto lock = frg::guard(&ptr->parent_->childrenMutex_);
		ptr->parent_->children_.push_back(ptr.get());
	}
	return ptr;
}

frg::vector<HierarchySnapshot, KernelAlloc> snapshotHierarchies() {
	frg::vector<HierarchySnapshot, KernelAlloc> snapshot{*kernelAlloc};

	// Stackless pre-order walk that pins one node at a time.
	// A pinned node is not finalized, so it and (via parent_) all of its ancestors are still linked.
	auto node = rootHierarchy();
	auto root = node.get();
	while(node) {
		snapshot.push_back({
			node->id(),
			node->parent() ? node->parent()->id() : 0,
			frg::string<KernelAlloc>{*kernelAlloc, node->tag()},
			node->chargedBytes()
		});

		smarter::shared_ptr<Hierarchy> next;
		{
			ScheduleGuard rcuGuard;

			// Try the children first, then the next siblings of the node and of its ancestors.
			// Dying nodes fail to lock() and can be skipped since they have no children left.
			auto parent = node.get();
			auto it = parent->children_.begin();
			while(true) {
				for(; it != parent->children_.end(); ++it) {
					next = (*it)->selfPtr_.lock();
					if(next)
						break;
				}
				if(next || parent == root)
					break;
				auto cursor = parent;
				parent = parent->parent_.get();
				it = parent->children_.iterator_to(cursor);
				++it;
			}
		}
		// Drops the previous pin outside of the RCU section.
		node = std::move(next);
	}

	return snapshot;
}

namespace {
	constinit frg::manual_box<smarter::shared_ptr<Hierarchy>> rootHierarchy_;

	initgraph::Task initRootHierarchy{&globalInitEngine, "generic.init-root-hierarchy",
		initgraph::Entails{getTaskingAvailableStage()},
		[] {
			auto created = Hierarchy::createRoot();
			if(!created)
				panicLogger() << "thor: Failed to allocate root hierarchy"
						<< frg::endlog;
			rootHierarchy_.initialize(std::move(*created));
		}
	};
}

smarter::shared_ptr<Hierarchy> rootHierarchy() {
	return *rootHierarchy_;
}

} // namespace thor
