#pragma once

#include <async/cancellation.hpp>
#include <frg/list.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

struct OneshotEvent;
struct BitsetEvent;

template <class Event>
requires (std::is_same_v<Event, OneshotEvent> || std::is_same_v<Event, BitsetEvent>)
struct AwaitEventNode {
	friend struct OneshotEvent;
	friend struct BitsetEvent;

	struct CancelFunctor {
		CancelFunctor(AwaitEventNode *node)
		: node_{node} { }

		inline void operator() () {
			node_->event_->cancelAwait(node_);
		}

	private:
		AwaitEventNode *node_;
	};

	void setup(Worklet *awaited, Event *event, async::cancellation_token cancelToken) {
		awaited_ = awaited;
		cancelToken_ = cancelToken;
		event_ = event;
	}

	Error error() const { return error_; }
	uint64_t sequence() const { return sequence_; }
	uint32_t bitset() const { return bitset_; }

	bool wasCancelled() const { return wasCancelled_; }

private:
	Worklet *awaited_;

	Error error_;
	uint64_t sequence_;
	uint32_t bitset_;

protected:
	bool wasCancelled_ = false;
	async::cancellation_observer<CancelFunctor> cancelCb_{this};
	async::cancellation_token cancelToken_;
	Event *event_;

private:

	frg::default_list_hook<AwaitEventNode> _queueNode;
};

struct OneshotEvent {
	OneshotEvent() = default;

	void trigger();

	void submitAwait(AwaitEventNode<OneshotEvent> *node, uint64_t sequence);
	void cancelAwait(AwaitEventNode<OneshotEvent> *node);

private:
	frg::ticket_spinlock _mutex;

	bool _triggered = false;

	// Protected by the sinkMutex.
	frg::intrusive_list<
		AwaitEventNode<OneshotEvent>,
		frg::locate_member<
			AwaitEventNode<OneshotEvent>,
			frg::default_list_hook<AwaitEventNode<OneshotEvent>>,
			&AwaitEventNode<OneshotEvent>::_queueNode
		>
	> _waitQueue;
};

struct BitsetEvent {
	BitsetEvent();

	void trigger(uint32_t bits);

	void submitAwait(AwaitEventNode<BitsetEvent> *node, uint64_t sequence);
	void cancelAwait(AwaitEventNode<BitsetEvent> *node);

private:
	frg::ticket_spinlock _mutex;

	uint64_t _lastTrigger[32];
	uint64_t _currentSequence;

	// Protected by the sinkMutex.
	frg::intrusive_list<
		AwaitEventNode<BitsetEvent>,
		frg::locate_member<
			AwaitEventNode<BitsetEvent>,
			frg::default_list_hook<AwaitEventNode<BitsetEvent>>,
			&AwaitEventNode<BitsetEvent>::_queueNode
		>
	> _waitQueue;
};

} // namespace thor
