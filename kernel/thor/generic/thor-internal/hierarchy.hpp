#pragma once

#include <atomic>
#include <expected>
#include <frg/list.hpp>
#include <frg/optional.hpp>
#include <frg/string.hpp>
#include <frg/vector.hpp>
#include <smarter.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/memory-controller.hpp>
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
		smarter::shared_ptr<Hierarchy> parent, frg::string<KernelAlloc> tag,
		frg::optional<size_t> memoryFloor
	);

	// If ownMemoryCtlr is null, the node uses the controller of its parent.
	Hierarchy(CtorToken, smarter::shared_ptr<Hierarchy> parent, frg::string<KernelAlloc> tag,
			frg::unique_ptr<MemoryController, AccountAllocator> ownMemoryCtlr);

	~Hierarchy();

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

	MemoryController *controller() const {
		return memoryCtlr_;
	}

	// Tracks the physical memory (in bytes) currently charged to this node.
	std::expected<void, Error> tryChargeMemory(size_t bytes);

	void unchargeMemory(size_t bytes);

	size_t chargedMemory() const {
		return chargedMemory_.load(std::memory_order_relaxed);
	}

	// Floor (in pages) of user-class allocations charged to this node.
	size_t memoryFloor() const {
		return memoryCtlr_->floor();
	}

	// Tracks the swap slots (in bytes) currently held by this node's memory views.
	// Their resident frames are charged (as physical memory) to the swap space's hierarchy.
	void chargeSwap(size_t bytes) {
		chargedSwap_.fetch_add(bytes, std::memory_order_relaxed);
	}
	void unchargeSwap(size_t bytes) {
		chargedSwap_.fetch_sub(bytes, std::memory_order_relaxed);
	}
	size_t chargedSwap() const {
		return chargedSwap_.load(std::memory_order_relaxed);
	}

private:
	uint64_t id_;
	smarter::shared_ptr<Hierarchy> parent_;
	frg::string<KernelAlloc> tag_;
	std::atomic<size_t> chargedMemory_{0};
	std::atomic<size_t> chargedSwap_{0};

	// Can be used to pin the node under RCU.
	smarter::weak_ptr<Hierarchy> selfPtr_;

	MemoryController *memoryCtlr_{nullptr};
	// Holds memoryCtlr_ if this node owns it.
	// Child hierarchy nodes keep this node alive so we do not need reference counting here.
	frg::unique_ptr<MemoryController, AccountAllocator> ownMemoryCtlr_;

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

// Allocates through a hierarchy's MemoryController and keeps the hierarchy alive.
// This is used for top-level objects (e.g., MemoryView overloads) that belong to a hierarchy.
struct HierarchyAllocator {
	explicit HierarchyAllocator(smarter::shared_ptr<Hierarchy> hierarchy)
	: hierarchy_{std::move(hierarchy)} { }

	void *allocate(size_t size) const {
		return AccountAllocator{hierarchy_->controller()}.allocate(size);
	}

	void deallocate(void *p, size_t size) const {
		kernelAlloc->deallocate(p, size);
	}

	void free(void *p) const {
		kernelAlloc->free(p);
	}

private:
	smarter::shared_ptr<Hierarchy> hierarchy_;
};

smarter::shared_ptr<Hierarchy> rootHierarchy();

// Flattened view of a single node, as handed out by snapshotHierarchies().
struct HierarchySnapshot {
	uint64_t id;
	uint64_t parentId;
	frg::string<KernelAlloc> tag;
	size_t chargedMemory;
	size_t chargedSwap;
};

// Debugging aid: lists all live hierarchy nodes, parents before children.
// Nodes that are created or destroyed concurrently may or may not be included.
frg::vector<HierarchySnapshot, KernelAlloc> snapshotHierarchies();

} // namespace thor
