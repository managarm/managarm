#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/schedule.hpp>

namespace thor {

static constexpr bool logTimers = false;
static constexpr bool logProgress = false;


namespace {

struct DeadlineState {
	frg::optional<uint64_t> timerDeadline{};
	frg::optional<uint64_t> preemptionDeadline{};

	frg::optional<uint64_t> currentDeadline{};
};

extern PerCpu<DeadlineState> deadlineState;
THOR_DEFINE_PERCPU(deadlineState);


void updateDeadline_() {
	assert(!intsAreEnabled());
	auto &state = deadlineState.get();

	frg::optional<uint64_t> deadline;
	auto consider = [&] (auto candidate) {
		if (!deadline)
			deadline = candidate;
		else if (candidate)
			deadline = frg::min(*deadline, *candidate);
	};

	consider(state.timerDeadline);
	consider(state.preemptionDeadline);

	// No need to do anything if the current deadline didn't change.

	// FIXME(qookie): This is just deadline == state.currentDeadline,
	// but frg::optional is missing the overload to do that.
	if (!deadline && !state.currentDeadline)
		return;
	if (deadline && state.currentDeadline && deadline == *state.currentDeadline)
		return;

	state.currentDeadline = deadline;
	setTimerDeadline(state.currentDeadline);
}

void setTimerEngineDeadline(frg::optional<uint64_t> deadline) {
	assert(!intsAreEnabled());
	deadlineState.get().timerDeadline = deadline;
	updateDeadline_();
}

} // namespace anonymous


void setPreemptionDeadline(frg::optional<uint64_t> deadline) {
	assert(!intsAreEnabled());
	deadlineState.get().preemptionDeadline = deadline;
	updateDeadline_();
}

 frg::optional<uint64_t> getPreemptionDeadline() {
	assert(!intsAreEnabled());
	return deadlineState.get().preemptionDeadline;
}


void handleTimerInterrupt() {
	auto &state = deadlineState.get();
	auto now = getClockNanos();

	// Clear all deadlines that have expired.
	auto checkAndClear = [&](frg::optional<uint64_t> &deadline) -> bool {
		if (!deadline || now < *deadline)
			return false;
		deadline = frg::null_opt;
		return true;
	};

	auto timerExpired = checkAndClear(state.timerDeadline);
	auto preemptionExpired = checkAndClear(state.preemptionDeadline);

	// Update the timer hardware.
	updateDeadline_();

	// Finally, take action for the deadlines that have expired.
	if (timerExpired)
		generalTimerEngine()->firedAlarm();

	if (preemptionExpired)
		localScheduler.get().forcePreemptionCall();
}


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

// This function unconditionally calls into setTimerEngineDeadline().
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
				setTimerEngineDeadline(frg::null_opt);
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
		setTimerEngineDeadline(_timerQueue.top()->_deadline);

		// We iterate if there was a race.
		// Technically, this is optional but it may help to avoid unnecessary IRQs.
		current = getClockNanos();
	} while(_timerQueue.top()->_deadline <= current);
}

PrecisionTimerEngine *generalTimerEngine() {
	return &timerEngine.get();
}

} // namespace thor
