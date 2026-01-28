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

	void setup(Worklet *awaited, Event *event, async::cancellation_token cancelToken, WorkQueue *wq) {
		awaited_ = awaited;
		cancelToken_ = cancelToken;
		event_ = event;
		wq_ = wq;
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
	WorkQueue *wq_;

private:

	frg::default_list_hook<AwaitEventNode> _queueNode;
};

struct OneshotEvent {
	OneshotEvent() = default;

	void trigger();

	void submitAwait(AwaitEventNode<OneshotEvent> *node, uint64_t sequence);
	void cancelAwait(AwaitEventNode<OneshotEvent> *node);

	// ----------------------------------------------------------------------------------
	// awaitEvent() and its boilerplate.
	// ----------------------------------------------------------------------------------

	struct AwaitEventResult {
		Error error;
		uint64_t sequence;
		uint32_t bitset;
	};

	template<typename Receiver>
	struct AwaitEventOperation : AwaitEventNode<OneshotEvent> {
		AwaitEventOperation(OneshotEvent *object, uint64_t sequence,
				async::cancellation_token cancelToken, WorkQueue *wq, Receiver r)
		: object_{object}, sequence_{sequence}, cancelToken_{cancelToken},
				wq_{wq}, r_{std::move(r)} { }

		void start() {
			worklet_.setup([] (Worklet *base) {
				auto self = frg::container_of(base, &AwaitEventOperation::worklet_);
				auto error = self->wasCancelled() ? Error::cancelled : self->error();
				async::execution::set_value(self->r_,
					AwaitEventResult{error, self->sequence(), self->bitset()});
			});
			setup(&worklet_, object_, cancelToken_, wq_);
			object_->submitAwait(this, sequence_);
		}

	private:
		OneshotEvent *object_;
		uint64_t sequence_;
		async::cancellation_token cancelToken_;
		WorkQueue *wq_;
		Receiver r_;
		Worklet worklet_;
	};

	struct AwaitEventSender {
		using value_type = AwaitEventResult;

		template<typename Receiver>
		friend AwaitEventOperation<Receiver> connect(AwaitEventSender s, Receiver r) {
			return {s.object, s.sequence, s.cancelToken, s.wq, std::move(r)};
		}

		friend async::sender_awaiter<AwaitEventSender, AwaitEventResult>
		operator co_await (AwaitEventSender s) {
			return {s};
		}

		OneshotEvent *object;
		uint64_t sequence;
		async::cancellation_token cancelToken;
		WorkQueue *wq;
	};

	AwaitEventSender awaitEvent(uint64_t sequence, async::cancellation_token cancelToken,
			WorkQueue *wq) {
		return {this, sequence, cancelToken, wq};
	}

	// ----------------------------------------------------------------------------------

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

	// ----------------------------------------------------------------------------------
	// awaitEvent() and its boilerplate.
	// ----------------------------------------------------------------------------------

	struct AwaitEventResult {
		Error error;
		uint64_t sequence;
		uint32_t bitset;
	};

	template<typename Receiver>
	struct AwaitEventOperation : AwaitEventNode<BitsetEvent> {
		AwaitEventOperation(BitsetEvent *object, uint64_t sequence,
				async::cancellation_token cancelToken, WorkQueue *wq, Receiver r)
		: object_{object}, sequence_{sequence}, cancelToken_{cancelToken},
				wq_{wq}, r_{std::move(r)} { }

		void start() {
			worklet_.setup([] (Worklet *base) {
				auto self = frg::container_of(base, &AwaitEventOperation::worklet_);
				auto error = self->wasCancelled() ? Error::cancelled : self->error();
				async::execution::set_value(self->r_,
					AwaitEventResult{error, self->sequence(), self->bitset()});
			});
			setup(&worklet_, object_, cancelToken_, wq_);
			object_->submitAwait(this, sequence_);
		}

	private:
		BitsetEvent *object_;
		uint64_t sequence_;
		async::cancellation_token cancelToken_;
		WorkQueue *wq_;
		Receiver r_;
		Worklet worklet_;
	};

	struct AwaitEventSender {
		using value_type = AwaitEventResult;

		template<typename Receiver>
		friend AwaitEventOperation<Receiver> connect(AwaitEventSender s, Receiver r) {
			return {s.object, s.sequence, s.cancelToken, s.wq, std::move(r)};
		}

		friend async::sender_awaiter<AwaitEventSender, AwaitEventResult>
		operator co_await (AwaitEventSender s) {
			return {s};
		}

		BitsetEvent *object;
		uint64_t sequence;
		async::cancellation_token cancelToken;
		WorkQueue *wq;
	};

	AwaitEventSender awaitEvent(uint64_t sequence, async::cancellation_token cancelToken,
			WorkQueue *wq) {
		return {this, sequence, cancelToken, wq};
	}

	// ----------------------------------------------------------------------------------

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
