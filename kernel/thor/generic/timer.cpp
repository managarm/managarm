
#include <frigg/debug.hpp>

#include "timer.hpp"
#include "../arch/x86/ints.hpp"

namespace thor {

static constexpr bool logTimers = false;
static constexpr bool logProgress = false;

ClockSource *globalClockSource;
PrecisionTimerEngine *globalTimerEngine;

void PrecisionTimerNode::cancelTimer() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_engine->_mutex);

	if(!_inQueue)
		return;
	_engine->_timerQueue.remove(this);
	_inQueue = false;
	_wasCancelled = true;
	_engine->_activeTimers--;
	WorkQueue::post(_elapsed);
}

PrecisionTimerEngine::PrecisionTimerEngine(ClockSource *clock, AlarmTracker *alarm)
: _clock{clock}, _alarm{alarm} {
	_alarm->setSink(this);
}

void PrecisionTimerEngine::installTimer(PrecisionTimerNode *timer) {
	assert(!timer->_engine);

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	if(logTimers) {
		auto current = _clock->currentNanos();
		frigg::infoLogger() << "thor: Setting timer at " << timer->_deadline
				<< " (counter is " << current << ")" << frigg::endLog;
	}

	_timerQueue.push(timer);
	_activeTimers++;
//	frigg::infoLogger() << "thor: Active timers: " << _activeTimers << frigg::endLog;

	timer->_engine = this;
	timer->_inQueue = true;

	_progress();
}

void PrecisionTimerEngine::firedAlarm() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	_progress();
}

// This function is somewhat complicated because we have to avoid a race between
// the comparator setup and the main counter.
void PrecisionTimerEngine::_progress() {
	auto current = _clock->currentNanos();
	do {
		// Process all timers that elapsed in the past.
		if(logProgress)
			frigg::infoLogger() << "thor: Processing timers until " << current << frigg::endLog;
		while(true) {
			if(_timerQueue.empty()) {
				_alarm->arm(0);
				return;
			}

			if(_timerQueue.top()->_deadline > current)
				break;

			auto timer = _timerQueue.top();
			_timerQueue.pop();
			assert(timer->_inQueue);
			timer->_inQueue = false;
			_activeTimers--;
			if(logProgress)
				frigg::infoLogger() << "thor: Timer completed" << frigg::endLog;
			WorkQueue::post(timer->_elapsed);
		}

		// Setup the comparator and iterate if there was a race.
		assert(!_timerQueue.empty());
		_alarm->arm(_timerQueue.top()->_deadline);
		current = _clock->currentNanos();
	} while(_timerQueue.top()->_deadline <= current);
}

ClockSource *systemClockSource() {
	return globalClockSource;
}

PrecisionTimerEngine *generalTimerEngine() {
	return globalTimerEngine;
}

} // namespace thor

