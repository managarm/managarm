
#include "cancel.hpp"
#include "core.hpp"

namespace thor {

CancelNode::CancelNode()
: _registry{}, _asyncId{0}, _cancelCalled{false} { }

void CancelNode::finalizeCancel() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_registry->_mutex);

	_registry->_nodeMap.remove(_asyncId);
}

CancelRegistry::CancelRegistry()
: _nodeMap{frigg::DefaultHasher<uint64_t>{}, *kernelAlloc}, _nextAsyncId{1} { }

void CancelRegistry::issue(CancelNode *node) {
	assert(!node->_registry && !node->_asyncId);
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	uint64_t id = _nextAsyncId++;
	_nodeMap.insert(id, node);

	node->_registry = _selfPtr.toShared();
	node->_asyncId = id;
}

void CancelRegistry::cancel(uint64_t async_id) {
	// TODO: We need QSGC here to prevent concurrent destruction of the node.
	CancelNode *node;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		// TODO: Return an error in this case.
		assert(async_id && async_id < _nextAsyncId);

		auto it = _nodeMap.get(async_id);
		if(!it)
			return;
		node = *it;
	}

	assert(!node->_cancelCalled);
	node->_cancelCalled = true;

	node->handleCancel();
}

} // namespace thor

