#pragma once

#include <atomic>
#include <expected>
#include <frg/list.hpp>
#include <frg/string.hpp>
#include <frg/vector.hpp>
#include <smarter.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/rcu.hpp>

namespace thor {

struct HierarchySnapshot;

struct Hierarchy : RcuProtected {
	friend frg::vector<HierarchySnapshot, KernelAlloc> snapshotHierarchies();

private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<Hierarchy>, Error> createRoot();

	static std::expected<smarter::shared_ptr<Hierarchy>, Error> extend(
		smarter::shared_ptr<Hierarchy> parent, frg::string<KernelAlloc> tag
	);

	Hierarchy(CtorToken, smarter::shared_ptr<Hierarchy> parent, frg::string<KernelAlloc> tag);

	// Unlinks this node from its parent; RCU keeps it traversable until the grace period ends.
	void finalizeBeforeRcu();

	uint64_t id() const {
		return id_;
	}

	smarter::borrowed_ptr<Hierarchy> parent() const {
		return parent_;
	}

	frg::string_view tag() const {
		return tag_;
	}

	// Tracks the physical memory (in bytes) currently charged to this node.
	void chargeMemory(size_t bytes) {
		chargedBytes_.fetch_add(bytes, std::memory_order_relaxed);
	}
	void unchargeMemory(size_t bytes) {
		chargedBytes_.fetch_sub(bytes, std::memory_order_relaxed);
	}
	size_t chargedBytes() const {
		return chargedBytes_.load(std::memory_order_relaxed);
	}

private:
	uint64_t id_;
	smarter::shared_ptr<Hierarchy> parent_;
	frg::string<KernelAlloc> tag_;
	std::atomic<size_t> chargedBytes_{0};

	// Can be used to pin the node under RCU.
	smarter::weak_ptr<Hierarchy> selfPtr_;

	IrqSpinlock childrenMutex_;

	// Protected against writes by the parent's childrenMutex_.
	frg::intrusive_rcu_list_hook<Hierarchy> siblingHook_;

	// Protected against writes by childrenMutex_.
	frg::intrusive_rcu_list<
		Hierarchy,
		frg::locate_member<
			Hierarchy,
			frg::intrusive_rcu_list_hook<Hierarchy>,
			&Hierarchy::siblingHook_
		>
	> children_;
};
static_assert(HasFinalizeBeforeRcu<Hierarchy>);

smarter::shared_ptr<Hierarchy> rootHierarchy();

// Flattened view of a single node, as handed out by snapshotHierarchies().
struct HierarchySnapshot {
	uint64_t id;
	uint64_t parentId;
	frg::string<KernelAlloc> tag;
	size_t chargedBytes;
};

// Debugging aid: lists all live hierarchy nodes, parents before children.
// Nodes that are created or destroyed concurrently may or may not be included.
frg::vector<HierarchySnapshot, KernelAlloc> snapshotHierarchies();

} // namespace thor
