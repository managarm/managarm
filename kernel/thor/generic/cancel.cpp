#include <thor-internal/cancel.hpp>
#include <thor-internal/core.hpp>

namespace thor {

CancelNode::CancelNode()
: _registry{}, _asyncId{0}, _cancelCalled{false} { }

CancelRegistry::CancelRegistry()
: _nodeMap{frg::hash<uint64_t>{}, *kernelAlloc}, _nextAsyncId{1} { }

void CancelRegistry::registerNode(CancelNode *node) {
	assert(!node->_registry && !node->_asyncId);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mapMutex);

	uint64_t id = _nextAsyncId++;
	_nodeMap.insert(id, node);

	node->_registry = _selfPtr.toShared();
	node->_asyncId = id;
}

void CancelRegistry::unregisterNode(CancelNode *node) {
	assert(node->_registry.get() == this && node->_asyncId);
	auto async_id = node->_asyncId;

	auto irq_lock = frg::guard(&irqMutex());
	auto cancel_lock = frg::guard(&_cancelMutex[async_id % lockGranularity]);
	auto map_lock = frg::guard(&_mapMutex);

	_nodeMap.remove(async_id);
}

void CancelRegistry::cancel(uint64_t async_id) {
	auto irq_lock = frg::guard(&irqMutex());
	auto cancel_lock = frg::guard(&_cancelMutex[async_id % lockGranularity]);

	// Hold the map mutex only to get a pointer to the node.
	// In particular, release it before calling handleCancellation().
	CancelNode *node;
	{
		auto map_lock = frg::guard(&_mapMutex);

		// TODO: Return an error in this case.
		assert(async_id && async_id < _nextAsyncId);

		auto it = _nodeMap.get(async_id);
		if(!it)
			return;
		node = *it;
	}

	// Because the _cancelMutex is still taken, the node cannot be freed here.
	assert(!node->_cancelCalled); // TODO: Return an error here.
	node->_cancelCalled = true;

	node->handleCancellation();
}

} // namespace thor
