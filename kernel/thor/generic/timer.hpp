#ifndef THOR_GENERIC_TIMER_HPP
#define THOR_GENERIC_TIMER_HPP

#include <atomic>

#include <frg/pairing_heap.hpp>
#include <frg/intrusive.hpp>
#include <frigg/atomic.hpp>
#include "cancel.hpp"
#include "execution/cancellation.hpp"
#include "work-queue.hpp"

namespace thor {

struct PrecisionTimerEngine;

struct ClockSource {
	virtual uint64_t currentNanos() = 0;
};

struct AlarmSink {
	virtual void firedAlarm() = 0;
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

	void setup(uint64_t deadline, cancellation_token cancelToken, Worklet *elapsed) {
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
	cancellation_token _cancelToken;
	Worklet *_elapsed;

	// TODO: If we allow timer engines to be destructed, this needs to be refcounted.
	PrecisionTimerEngine *_engine;

	TimerState _state = TimerState::none;
	bool _wasCancelled = false;
	transient_cancellation_callback<CancelFunctor> _cancelCb;
};

struct CompareTimer {
	bool operator() (const PrecisionTimerNode *a, const PrecisionTimerNode *b) const {
		return a->_deadline > b->_deadline;
	}
};

struct PrecisionTimerEngine : private AlarmSink {
	friend struct PrecisionTimerNode;

private:
	using Mutex = frigg::TicketLock;

public:
	PrecisionTimerEngine(ClockSource *clock, AlarmTracker *alarm);
	
	void installTimer(PrecisionTimerNode *timer);

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

ClockSource *systemClockSource();
PrecisionTimerEngine *generalTimerEngine();

} // namespace thor

#endif // THOR_GENERIC_TIMER_HPP
