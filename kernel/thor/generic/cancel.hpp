#ifndef THOR_GENERIC_CANCEL_HPP
#define THOR_GENERIC_CANCEL_HPP

#include <stdint.h>

#include <frigg/hashmap.hpp>
#include <frigg/smart_ptr.hpp>
#include "kernel_heap.hpp"

namespace thor {

struct CancelRegistry;

struct CancelNode {
	friend struct CancelRegistry;

	CancelNode();

	CancelNode(const CancelNode &) = delete;

	CancelNode &operator= (const CancelNode &) = delete;

	uint64_t asyncId() {
		return _asyncId;
	}

protected:
	virtual void handleCancel() = 0;

	void finalizeCancel();

private:
	frigg::SharedPtr<CancelRegistry> _registry;

	uint64_t _asyncId;

	bool _cancelCalled;
};

struct CancelRegistry {
	friend struct CancelNode;

public:
	CancelRegistry();

	CancelRegistry(const CancelRegistry &) = delete;

	CancelRegistry &operator= (const CancelRegistry &) = delete;

	void setupSelfPtr(frigg::UnsafePtr<CancelRegistry> ptr) {
		_selfPtr = ptr;
	}

	void issue(CancelNode *node);

	void cancel(uint64_t async_id);

private:
	frigg::UnsafePtr<CancelRegistry> _selfPtr;

	frigg::TicketLock _mutex;

	frigg::Hashmap<uint64_t, CancelNode *,
			frigg::DefaultHasher<uint64_t>, KernelAlloc> _nodeMap;

	uint64_t _nextAsyncId;

};

} // namespace thor

#endif // THOR_GENERIC_CANCEL_HPP
