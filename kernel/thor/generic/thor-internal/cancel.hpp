#pragma once

#include <stdint.h>

#include <atomic>
#include <assert.h>
#include <async/cancellation.hpp>
#include <frg/list.hpp>
#include <frg/rbtree.hpp>
#include <thor-internal/kernel-heap.hpp>

namespace thor {

// Private helper struct for CancelRegistry.
struct CancelNode {
	uint64_t tag;
	std::atomic<int> refcount{0};
	bool cancelled = false;
	async::cancellation_event event;
	frg::rbtree_hook treeNode;
	frg::default_list_hook<CancelNode> listHook;
};

struct CancelNodeLess {
	bool operator() (const CancelNode &a, const CancelNode &b) {
		return a.tag < b.tag;
	}
};

using CancelNodeTree = frg::rbtree<CancelNode, &CancelNode::treeNode, CancelNodeLess>;

struct CancelGuard {
	friend void swap(CancelGuard &a, CancelGuard &b) {
		std::swap(a.node_, b.node_);
	}

	CancelGuard() : node_{nullptr} {}

	explicit CancelGuard(CancelNode *node) : node_{node} {}

	CancelGuard(CancelGuard &&other) : CancelGuard() {
		swap(*this, other);
	}

	CancelGuard(const CancelGuard &) = delete;

	CancelGuard &operator=(CancelGuard other) {
		swap(*this, other);
		return *this;
	}

	// CancelGuard must be consumed by CancelRegistry::unregisterTag().
	~CancelGuard() {
		assert(!node_);
	}

	async::cancellation_token token() const {
		if (!node_)
			return {};
		return node_->event;
	}

private:
	friend struct CancelRegistry;
	CancelNode *node_;
};

struct CancelRegistry {
public:
	CancelRegistry();

	CancelRegistry(const CancelRegistry &) = delete;

	CancelRegistry &operator= (const CancelRegistry &) = delete;

	CancelGuard registerTag(uint64_t cancellationTag);

	void unregisterTag(CancelGuard guard);

	// Returns the number of cancelled operations.
	unsigned int cancel(uint64_t cancellationTag);

private:
	frg::ticket_spinlock mutex_;

	CancelNodeTree tree_;
};

} // namespace thor
