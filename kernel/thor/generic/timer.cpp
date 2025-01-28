#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

static constexpr bool logTimers = false;
static constexpr bool logProgress = false;

extern PerCpu<PrecisionTimerEngine> timerEngine;
THOR_DEFINE_PERCPU(timerEngine);

void PrecisionTimerEngine::installTimer(PrecisionTimerNode *timer) {
	assert(!timer->_engine);
	timer->_engine = this;

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	assert(timer->_state == TimerState::none);

	if(logTimers) {
		auto current = getClockNanos();
		infoLogger() << "thor: Setting timer at " << timer->_deadline
				<< " (counter is " << current << ")" << frg::endlog;
	}

//	infoLogger() << "thor: Active timers: " << _activeTimers << frg::endlog;

	if(!timer->_cancelCb.try_set(timer->_cancelToken)) {
		timer->_wasCancelled = true;
		timer->_state = TimerState::retired;
		WorkQueue::post(timer->_elapsed);
		return;
	}

	_timerQueue.push(timer);
	_activeTimers++;
	timer->_state = TimerState::queued;

	_progress();
}

void PrecisionTimerEngine::cancelTimer(PrecisionTimerNode *timer) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	if(timer->_state == TimerState::queued) {
		_timerQueue.remove(timer);
		_activeTimers--;
		timer->_wasCancelled = true;
	}else{
		assert(timer->_state == TimerState::elapsed);
	}

	timer->_state = TimerState::retired;
	WorkQueue::post(timer->_elapsed);
}

void PrecisionTimerEngine::firedAlarm() {
	assert(getCpuData() == _ourCpu);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	_progress();
}

// This function unconditionally calls into setTimerDeadline().
// This is necessary since we assume that timer IRQs are one shot
// and not necessarily perfectly accurate.
void PrecisionTimerEngine::_progress() {
	assert(getCpuData() == _ourCpu);

	auto current = getClockNanos();
	do {
		// Process all timers that elapsed in the past.
		if(logProgress)
			infoLogger() << "thor: Processing timers until " << current << frg::endlog;
		while(true) {
			if(_timerQueue.empty()) {
				setTimerDeadline(frg::null_opt);
				return;
			}

			if(_timerQueue.top()->_deadline > current)
				break;

			auto timer = _timerQueue.top();
			assert(timer->_state == TimerState::queued);
			_timerQueue.pop();
			_activeTimers--;
			if(logProgress)
				infoLogger() << "thor: Timer completed" << frg::endlog;
			if(timer->_cancelCb.try_reset()) {
				timer->_state = TimerState::retired;
				WorkQueue::post(timer->_elapsed);
			}else{
				// Let the cancellation handler invoke the continuation.
				timer->_state = TimerState::elapsed;
			}
		}

		// Setup the interrupt.
		assert(!_timerQueue.empty());
		setTimerDeadline(_timerQueue.top()->_deadline);

		// We iterate if there was a race.
		// Technically, this is optional but it may help to avoid unnecessary IRQs.
		current = getClockNanos();
	} while(_timerQueue.top()->_deadline <= current);
}

PrecisionTimerEngine *generalTimerEngine() {
	return &timerEngine.get();
}

} // namespace thor
