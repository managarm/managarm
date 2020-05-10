#ifndef THOR_GENERIC_TIMER_HPP
#define THOR_GENERIC_TIMER_HPP

#include <atomic>

#include <frg/container_of.hpp>
#include <frg/pairing_heap.hpp>
#include <frg/intrusive.hpp>
#include <frigg/atomic.hpp>
#include "cancel.hpp"
#include "execution/basics.hpp"
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

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for sleep()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct SleepOperation;

	struct [[nodiscard]] SleepSender {
		template<typename R>
		friend SleepOperation<R>
		connect(SleepSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		PrecisionTimerEngine *self;
		uint64_t deadline;
		cancellation_token cancellation;
	};

	SleepSender sleep(uint64_t deadline, cancellation_token cancellation = {}) {
		return {this, deadline, cancellation};
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
				op->receiver_.set_value();
			});
			node_.setup(s_.deadline, &worklet_);
			s_.self->installTimer(&node_);
		}

	private:
		SleepSender s_;
		R receiver_;
		PrecisionTimerNode node_;
		Worklet worklet_;
	};

	friend execution::sender_awaiter<SleepSender>
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

ClockSource *systemClockSource();
PrecisionTimerEngine *generalTimerEngine();

} // namespace thor

#endif // THOR_GENERIC_TIMER_HPP
