
#include <frigg/debug.hpp>

#include "timer.hpp"
#include "../arch/x86/ints.hpp"

namespace thor {

static constexpr bool logTimers = false;
static constexpr bool logProgress = false;

ClockSource *globalClockSource;
PrecisionTimerEngine *globalTimerEngine;

PrecisionTimerEngine::PrecisionTimerEngine(ClockSource *clock, AlarmTracker *alarm)
: _clock{clock}, _alarm{alarm} {
	_alarm->setSink(this);
}

void PrecisionTimerEngine::installTimer(PrecisionTimerNode *timer) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	if(logTimers) {
		auto current = _clock->currentNanos();
		frigg::infoLogger() << "thor: Setting timer at " << timer->deadline
				<< " (counter is " << current << ")" << frigg::endLog;
	}

	_timerQueue.push(timer);
	_activeTimers++;
//		frigg::infoLogger() << "hpet: Active timers: " << _activeTimers << frigg::endLog;
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
			if(_timerQueue.empty())
				return;

			if(_timerQueue.top()->deadline > current)
				break;

			auto timer = _timerQueue.top();
			_timerQueue.pop();
			_activeTimers--;
			if(logProgress)
				frigg::infoLogger() << "thor: Timer completed" << frigg::endLog;
			timer->onElapse();
		}

		// Setup the comparator and iterate if there was a race.
		assert(!_timerQueue.empty());
		_alarm->arm(_timerQueue.top()->deadline);
		current = _clock->currentNanos();
	} while(_timerQueue.top()->deadline <= current);
}

ClockSource *systemClockSource() {
	return globalClockSource;
}

PrecisionTimerEngine *generalTimerEngine() {
	return globalTimerEngine;
}

} // namespace thor

