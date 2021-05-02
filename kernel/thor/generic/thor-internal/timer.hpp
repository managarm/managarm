#pragma once

#include <atomic>

#include <async/basic.hpp>
#include <async/cancellation.hpp>
#include <frg/container_of.hpp>
#include <frg/intrusive.hpp>
#include <frg/pairing_heap.hpp>
#include <frg/spinlock.hpp>
#include <thor-internal/cancel.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

struct PrecisionTimerEngine;

struct ClockSource {
	virtual uint64_t currentNanos() = 0;

protected:
	~ClockSource() = default;
};

ClockSource *systemClockSource();

struct AlarmSink {
	virtual void firedAlarm() = 0;

protected:
	~AlarmSink() = default;
};

struct AlarmTracker {
	AlarmTracker()
	: _sink{nullptr} { }

	void setSink(AlarmSink *sink) {
		assert(!_sink.load(std::memory_order_relaxed));
		_sink.store(sink, std::memory_order_release);
	}

	virtual void arm(uint64_t nanos) = 0;

protected:
	void fireAlarm() {
		auto sink = _sink.load(std::memory_order_acquire);
		if(sink)
			sink->firedAlarm();
	}

	~AlarmTracker() = default;

private:
	std::atomic<AlarmSink *> _sink;
};

enum class TimerState {
	none,
	queued,
	elapsed,
	retired
};

struct PrecisionTimerNode {
	struct CancelFunctor {
		CancelFunctor(PrecisionTimerNode *node)
		: node_{node} { }

		void operator() ();

	private:
		PrecisionTimerNode *node_;
	};

	friend struct CompareTimer;
	friend struct PrecisionTimerEngine;

	PrecisionTimerNode()
	: _engine{nullptr}, _cancelCb{this} { }

	void setup(uint64_t deadline, Worklet *elapsed) {
		_deadline = deadline;
		_elapsed = elapsed;
	}

	void setup(uint64_t deadline, async::cancellation_token cancelToken, Worklet *elapsed) {
		_deadline = deadline;
		_cancelToken = cancelToken;
		_elapsed = elapsed;
	}

	bool wasCancelled() {
		return _wasCancelled;
	}

	frg::pairing_heap_hook<PrecisionTimerNode> hook;

private:
	uint64_t _deadline;
	async::cancellation_token _cancelToken;
	Worklet *_elapsed;

	// TODO: If we allow timer engines to be destructed, this needs to be refcounted.
	PrecisionTimerEngine *_engine;

	TimerState _state = TimerState::none;
	bool _wasCancelled = false;
	async::cancellation_observer<CancelFunctor> _cancelCb;
};

struct CompareTimer {
	bool operator() (const PrecisionTimerNode *a, const PrecisionTimerNode *b) const {
		return a->_deadline > b->_deadline;
	}
};

struct PrecisionTimerEngine final : private AlarmSink {
	friend struct PrecisionTimerNode;

private:
	using Mutex = frg::ticket_spinlock;

public:
	PrecisionTimerEngine(ClockSource *clock, AlarmTracker *alarm);
	
	void installTimer(PrecisionTimerNode *timer);

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for sleep()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct SleepOperation;

	struct [[nodiscard]] SleepSender {
		using value_type = void;

		template<typename R>
		friend SleepOperation<R>
		connect(SleepSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		PrecisionTimerEngine *self;
		uint64_t deadline;
		async::cancellation_token cancellation;
	};

	SleepSender sleep(uint64_t deadline, async::cancellation_token cancellation = {}) {
		return {this, deadline, cancellation};
	}

	SleepSender sleepFor(uint64_t nanos, async::cancellation_token cancellation = {}) {
		return {this, systemClockSource()->currentNanos() + nanos, cancellation};
	}

	template<typename R>
	struct SleepOperation {
		SleepOperation(SleepSender s, R receiver)
		: s_{std::move(s)}, receiver_{std::move(receiver)} { }

		SleepOperation(const SleepOperation &) = delete;

		SleepOperation &operator= (const SleepOperation &) = delete;

		void start() {
			worklet_.setup([] (Worklet *base) {
				auto op = frg::container_of(base, &SleepOperation::worklet_);
				async::execution::set_value(op->receiver_);
			}, WorkQueue::generalQueue());
			node_.setup(s_.deadline, &worklet_);
			s_.self->installTimer(&node_);
		}

	private:
		SleepSender s_;
		R receiver_;
		PrecisionTimerNode node_;
		Worklet worklet_;
	};

	friend async::sender_awaiter<SleepSender>
	operator co_await(SleepSender sender) {
		return {std::move(sender)};
	}

	// ----------------------------------------------------------------------------------

private:
	void cancelTimer(PrecisionTimerNode *timer);

	void firedAlarm();

private:
	void _progress();

	ClockSource *_clock;
	AlarmTracker *_alarm;

	Mutex _mutex;

	frg::pairing_heap<
		PrecisionTimerNode,
		frg::locate_member<
			PrecisionTimerNode,
			frg::pairing_heap_hook<PrecisionTimerNode>,
			&PrecisionTimerNode::hook
		>,
		CompareTimer
	> _timerQueue;
	
	size_t _activeTimers;
};

inline void PrecisionTimerNode::CancelFunctor::operator() () {
	node_->_engine->cancelTimer(node_);
}

PrecisionTimerEngine *generalTimerEngine();

bool haveTimer();

} // namespace thor
