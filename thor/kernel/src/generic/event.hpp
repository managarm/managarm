#ifndef THOR_EVENT_HPP
#define THOR_EVENT_HPP

#include <frg/list.hpp>
#include "error.hpp"
#include "work-queue.hpp"

namespace thor {

struct AwaitBitsetNode {
	friend struct BitsetEvent;

	void setup(Worklet *awaited) {
		_awaited = awaited;
	}

	Error error() { return _error; }
	uint64_t sequence() { return _sequence; }
	uint32_t bitset() { return _bitset; }

private:
	Worklet *_awaited;

	Error _error;
	uint64_t _sequence;
	uint32_t _bitset;

	frg::default_list_hook<AwaitBitsetNode> _queueNode;
};

struct BitsetEvent {
	BitsetEvent();

	void trigger(uint32_t bits);

	void submitAwait(AwaitBitsetNode *node, uint64_t sequence);

private:
	frigg::TicketLock _mutex;

	uint64_t _lastTrigger[32];
	uint64_t _currentSequence;

	// Protected by the sinkMutex.
	frg::intrusive_list<
		AwaitBitsetNode,
		frg::locate_member<
			AwaitBitsetNode,
			frg::default_list_hook<AwaitBitsetNode>,
			&AwaitBitsetNode::_queueNode
		>
	> _waitQueue;
};

} // namespace thor

#endif // THOR_EVENT_HPP
