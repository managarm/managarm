#include <thor-internal/cancel.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

CancelRegistry::CancelRegistry() { }

CancelGuard CancelRegistry::registerTag(uint64_t cancellationTag) {
	if (!cancellationTag)
		return {};

	// TODO: It may be possible to do this without allocations,
	//       e.g., by embedding the Node into a coroutine frame.
	//       One possibility would be using a specialized coroutine promise type
	//       that embeds the Node.

	auto node = frg::construct<CancelNode>(*kernelAlloc);
	node->tag = cancellationTag;
	node->refcount.store(1, std::memory_order_relaxed);

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		tree_.insert(node);
	}

	return CancelGuard{node};
}

void CancelRegistry::unregisterTag(CancelGuard guard) {
	auto node = guard.node_;
	if (!node)
		return;
	guard.node_ = nullptr;

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		tree_.remove(node);
	}

	if (node->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
		frg::destruct(*kernelAlloc, node);
}

unsigned int CancelRegistry::cancel(uint64_t cancellationTag) {
	if (!cancellationTag)
		return 0;

	frg::intrusive_list<
		CancelNode,
		frg::locate_member<
			CancelNode,
			frg::default_list_hook<CancelNode>,
			&CancelNode::listHook
		>
	> pending;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		// Find leftmost node with this tag by walking down the tree.
		CancelNode *leftmost = nullptr;
		auto current = tree_.get_root();
		while (current) {
			if (cancellationTag < current->tag) {
				current = CancelNodeTree::get_left(current);
			} else if (cancellationTag > current->tag) {
				current = CancelNodeTree::get_right(current);
			} else {
				// Found a match. Continue left to find leftmost.
				leftmost = current;
				current = CancelNodeTree::get_left(current);
			}
		}

		for (auto it = leftmost; it && it->tag == cancellationTag; it = CancelNodeTree::successor(it)) {
			// Make sure to not enqueue the same Node multiple times.
			if (it->cancelled)
				continue;
			it->cancelled = true;
			it->refcount.fetch_add(1, std::memory_order_relaxed);
			pending.push_back(it);
		}
	}

	unsigned int count = 0;
	while (!pending.empty()) {
		auto node = pending.pop_front();
		node->event.cancel();

		if (node->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
			frg::destruct(*kernelAlloc, node);
		++count;
	}

	return count;
}

} // namespace thor
