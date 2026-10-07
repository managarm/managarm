#pragma once

#include <atomic>
#include <expected>

#include <async/cancellation.hpp>
#include <async/recurring-event.hpp>
#include <frg/expected.hpp>
#include <frg/list.hpp>
#include <frg/tuple.hpp>
#include <smarter.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/rcu-base.hpp>
#include <thor-internal/universe.hpp>
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

struct OneshotEvent : RcuProtected {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<OneshotEvent>, Error> create();

	OneshotEvent(CtorToken) { }

	std::expected<void, Error> trigger();

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

struct BitsetEvent : RcuProtected {
private:
	struct CtorToken {};

public:
	static std::expected<smarter::shared_ptr<BitsetEvent>, Error> create();

	BitsetEvent(CtorToken);

	std::expected<void, Error> trigger(uint32_t bits);

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

// Event that counts how often it was raised.
// The wait capability and the raise capability refer to different ends (i.e., peers)
// of the event such that each side notices when the other side is gone.
// Memory ordering: if awaitEvent() returns n, everything that happened before the raise()
// calls that produced sequence numbers 1 to n happens before awaitEvent() returns.
// If it fails with endOfLane, the same holds for all raises.
struct SequencedEvent final : RcuProtected, TwoPeerObject {
private:
	struct CtorToken {};

public:
	static constexpr int waitEnd = 0;
	static constexpr int raiseEnd = 1;

	// Returns the wait capability and the raise capability (in this order).
	static std::expected<
		frg::tuple<
			smarter::shared_ptr<SequencedEvent, TwoPeerPolicy>,
			smarter::shared_ptr<SequencedEvent, TwoPeerPolicy>
		>,
		Error
	> create();

	SequencedEvent(CtorToken) { }

	// Number of times that the event was raised so far.
	uint64_t sequence();

	// Fails with endOfLane if the wait end is gone.
	std::expected<void, Error> raise();

	// Completes with the current sequence number once it exceeds the given one.
	// Fails with endOfLane if that cannot happen anymore since the raise end is gone.
	coroutine<frg::expected<Error, uint64_t>> awaitEvent(uint64_t sequence,
			async::cancellation_token cancelToken);

private:
	// Called after the counter of an end reached zero.
	void onPeersZero(int end) override;

	std::atomic<uint64_t> sequence_{0};
	std::atomic<bool> endGone_[2] = {false, false};

	async::recurring_event event_;
};

} // namespace thor
