#ifndef THOR_GENERIC_TIMER_HPP
#define THOR_GENERIC_TIMER_HPP

#include <atomic>

#include <frg/pairing_heap.hpp>
#include <frg/intrusive.hpp>
#include <frigg/atomic.hpp>

namespace thor {

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

struct PrecisionTimerNode {
	PrecisionTimerNode(uint64_t deadline)
	: deadline{deadline} { }

	// The timer subsystem drops its references to the node before this call.
	virtual void onElapse() = 0;

	uint64_t deadline;

	frg::pairing_heap_hook<PrecisionTimerNode> hook;
};

struct CompareTimer {
	bool operator() (const PrecisionTimerNode *a, const PrecisionTimerNode *b) const {
		return a->deadline > b->deadline;
	}
};

struct PrecisionTimerEngine : private AlarmSink {
private:
	using Mutex = frigg::TicketLock;

public:
	PrecisionTimerEngine(ClockSource *clock, AlarmTracker *alarm);
	
	void installTimer(PrecisionTimerNode *timer);

private:
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

ClockSource *systemClockSource();
PrecisionTimerEngine *generalTimerEngine();

} // namespace thor

#endif // THOR_GENERIC_TIMER_HPP
