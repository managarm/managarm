#pragma once

#include <frg/list.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

struct AwaitEventNode {
	friend struct OneshotEvent;
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

	frg::default_list_hook<AwaitEventNode> _queueNode;
};

struct OneshotEvent {
	OneshotEvent() = default;

	void trigger();

	void submitAwait(AwaitEventNode *node, uint64_t sequence);

private:
	frigg::TicketLock _mutex;

	bool _triggered = false;

	// Protected by the sinkMutex.
	frg::intrusive_list<
		AwaitEventNode,
		frg::locate_member<
			AwaitEventNode,
			frg::default_list_hook<AwaitEventNode>,
			&AwaitEventNode::_queueNode
		>
	> _waitQueue;
};

struct BitsetEvent {
	BitsetEvent();

	void trigger(uint32_t bits);

	void submitAwait(AwaitEventNode *node, uint64_t sequence);

private:
	frigg::TicketLock _mutex;

	uint64_t _lastTrigger[32];
	uint64_t _currentSequence;

	// Protected by the sinkMutex.
	frg::intrusive_list<
		AwaitEventNode,
		frg::locate_member<
			AwaitEventNode,
			frg::default_list_hook<AwaitEventNode>,
			&AwaitEventNode::_queueNode
		>
	> _waitQueue;
};

} // namespace thor
