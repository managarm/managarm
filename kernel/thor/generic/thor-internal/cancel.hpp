#pragma once

#include <stdint.h>

#include <assert.h>
#include <frg/hash_map.hpp>
#include <smarter.hpp>
#include <thor-internal/kernel_heap.hpp>

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
	// This function is called to perform cancellation.
	// Note that it is called with CancelRegistry::_cancelLock taken, so it must cancel the
	// operation quickly without doing heavy work.
	// Specifically, it must not call into CancelRegistry functions.
	virtual void handleCancellation() = 0;

	~CancelNode() = default;

private:
	smarter::shared_ptr<CancelRegistry> _registry;

	uint64_t _asyncId;

	bool _cancelCalled;
};

struct CancelRegistry {
	friend struct CancelNode;

public:
	CancelRegistry();

	CancelRegistry(const CancelRegistry &) = delete;

	CancelRegistry &operator= (const CancelRegistry &) = delete;

	void setupSelfPtr(smarter::borrowed_ptr<CancelRegistry> ptr) {
		_selfPtr = ptr;
	}

	void registerNode(CancelNode *node);
	void unregisterNode(CancelNode *node);

	void cancel(uint64_t async_id);

private:
	// Number of _cancelMutex instances.
	// Can be adjusted to tune the scalability of this mechanism.
	static constexpr int lockGranularity = 4;

	smarter::borrowed_ptr<CancelRegistry> _selfPtr;

	// Protects the cancel operation.
	// This is indexed by the asyncId of the operation.
	// Taken *before* _mapMutex.
	frg::ticket_spinlock _cancelMutex[lockGranularity];

	// Protects the _nodeMap.
	frg::ticket_spinlock _mapMutex;

	frg::hash_map<
		uint64_t,
		CancelNode *,
		frg::hash<uint64_t>,
		KernelAlloc
	> _nodeMap;

	uint64_t _nextAsyncId;

};

} // namespace thor
