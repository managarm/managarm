
#include "kernel.hpp"

namespace thor {

namespace {
	constexpr bool logScheduling = false;
	constexpr bool logNextBest = false;
	constexpr bool logUpdates = false;
	constexpr bool logTimeSlice = false;

	// Minimum length of a preemption time slice in ns.
	static constexpr int64_t sliceGranularity = 10'000'000;
}

int ScheduleEntity::orderPriority(const ScheduleEntity *a, const ScheduleEntity *b) {
	return b->priority - a->priority; // Prefer larger priority.
}

bool ScheduleEntity::scheduleBefore(const ScheduleEntity *a, const ScheduleEntity *b) {
	return a->baseUnfairness - a->refProgress
			> b->baseUnfairness - b->refProgress; // Prefer greater unfairness.
}

ScheduleEntity::ScheduleEntity()
: state{ScheduleState::null}, priority{0}, _refClock{0}, _runTime{0},
		refProgress{0}, baseUnfairness{0} { }

ScheduleEntity::~ScheduleEntity() {
	assert(state == ScheduleState::null);
}

void Scheduler::associate(ScheduleEntity *entity, Scheduler *scheduler) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&scheduler->_mutex);

//	frigg::infoLogger() << "associate " << entity << frigg::endLog;
	assert(entity->state == ScheduleState::null);
	entity->_scheduler = scheduler;
	entity->state = ScheduleState::attached;
}

void Scheduler::unassociate(ScheduleEntity *entity) {
	auto irq_lock = frigg::guard(&irqMutex());
	
	auto self = entity->_scheduler;
	assert(self);
	auto lock = frigg::guard(&self->_mutex);

	assert(entity->state == ScheduleState::attached);
	assert(entity != self->_current);
	entity->_scheduler = nullptr;
	entity->state = ScheduleState::null;
}

void Scheduler::setPriority(ScheduleEntity *entity, int priority) {
	auto irq_lock = frigg::guard(&irqMutex());

	auto self = entity->_scheduler;
	assert(self);
	auto lock = frigg::guard(&self->_mutex);

	// Otherwise, we would have to remove-reinsert into the queue.
	assert(entity == self->_current);

	entity->priority = priority;
}

void Scheduler::resume(ScheduleEntity *entity) {
	auto irq_lock = frigg::guard(&irqMutex());

//	frigg::infoLogger() << "resume " << entity << frigg::endLog;
	assert(entity->state == ScheduleState::attached);

	auto self = entity->_scheduler;
	assert(self);
	auto lock = frigg::guard(&self->_mutex);
	assert(entity != self->_current);

	self->_updateSystemProgress();

	// Update the unfairness reference on resume.
	if(self->_current)
		self->_updateCurrentEntity();
	entity->refProgress = self->_systemProgress;
	entity->_refClock = self->_refClock;
	entity->state = ScheduleState::active;
	
	self->_waitQueue.push(entity);
	self->_numWaiting++;

	if(self == &getCpuData()->scheduler) {
		self->_updatePreemption();
	}else{
		sendPingIpi(self->_cpuContext->localApicId);
	}
}

void Scheduler::suspendCurrent() {
	auto irq_lock = frigg::guard(&irqMutex());

	auto self = localScheduler();
	auto lock = frigg::guard(&self->_mutex);
	auto entity = self->_current;
	assert(entity);
//	frigg::infoLogger() << "suspend " << entity << frigg::endLog;
	
	self->_updateSystemProgress();

	// Update the unfairness on suspend.
	self->_updateCurrentEntity();
	self->_updateEntityStats(entity);
	entity->state = ScheduleState::attached;

	self->_current = nullptr;
}

void Scheduler::suspendWaiting(ScheduleEntity *entity) {
	auto irq_lock = frigg::guard(&irqMutex());

//	frigg::infoLogger() << "suspend " << entity << frigg::endLog;
	assert(entity->state == ScheduleState::active);
	
	auto self = entity->_scheduler;
	assert(self);
	auto lock = frigg::guard(&self->_mutex);
	assert(entity != self->_current);

	assert(!"This function is untested");
	
	self->_updateSystemProgress();

	// Update the unfairness on suspend.
	self->_updateWaitingEntity(entity);
	self->_updateEntityStats(entity);
	entity->state = ScheduleState::attached;

	self->_waitQueue.remove(entity); // TODO: Pairing heap remove() is untested.
	self->_numWaiting--;

	if(self == &getCpuData()->scheduler) {
		self->_updatePreemption();
	}else{
		sendPingIpi(self->_cpuContext->localApicId);
	}
}

Scheduler::Scheduler(CpuData *cpu_context)
: _cpuContext{cpu_context}, _scheduleFlag{false}, _current{nullptr},
		_numWaiting{0}, _refClock{0}, _systemProgress{0} { }

Progress Scheduler::_liveUnfairness(const ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::active);

	auto delta_progress = _systemProgress - entity->refProgress;
	if(entity == _current) {
		return entity->baseUnfairness - _numWaiting * delta_progress;
	}else{
		return entity->baseUnfairness + delta_progress;
	}
}

int64_t Scheduler::_liveRuntime(const ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::active);
	if(entity == _current) {
		return entity->_runTime + (_refClock - entity->_refClock);
	}else{
		return entity->_runTime;
	}
}

bool Scheduler::wantSchedule() {
	assert(!intsAreEnabled());
	auto lock = frigg::guard(&_mutex);

	_updateSystemProgress();
	_refreshFlag();
	_updatePreemption();
	return _scheduleFlag;
}

void Scheduler::reschedule() {
	assert(!intsAreEnabled());
	auto lock = frigg::guard(&_mutex);

	_updateSystemProgress();

	if(_current)
		_unschedule();
	
	_sliceClock = _refClock;
	
	if(_waitQueue.empty()) {
		if(logScheduling)
			frigg::infoLogger() << "System is idle" << frigg::endLog;
		lock.unlock();
		suspendSelf();
		frigg::panicLogger() << "Return from suspendSelf()" << frigg::endLog;
	}

	_schedule();
	assert(_current);

	_updatePreemption();

	lock.unlock();
	_current->invoke();
	frigg::panicLogger() << "Return from ScheduleEntity::invoke()" << frigg::endLog;
	__builtin_unreachable();
}

void Scheduler::_unschedule() {
	assert(_current);

	// Decrease the unfairness at the end of the time slice.
	_updateCurrentEntity();
	_updateEntityStats(_current);

	if(_current->state == ScheduleState::active) {
		_waitQueue.push(_current);
		_numWaiting++;
	}

	_current = nullptr;
}

void Scheduler::_schedule() {
	assert(!_current);

	assert(!_waitQueue.empty());
	auto entity = _waitQueue.top();
	_waitQueue.pop();
	_numWaiting--;

	// Increase the unfairness at the start of the time slice.
	assert(entity->state == ScheduleState::active);
	_updateWaitingEntity(entity);
	_updateEntityStats(entity);

	if(logScheduling) {
//		frigg::infoLogger() << "System progress: " << (_systemProgress / 256) / (1000 * 1000)
//				<< " ms" << frigg::endLog;
		frigg::infoLogger() << "Running entity with priority: " << entity->priority
				<< ", unfairness: " << (_liveUnfairness(entity) / 256) / (1000 * 1000)
				<< " ms, runtime: " << _liveRuntime(entity) / (1000 * 1000)
				<< " ms (" << (_numWaiting + 1) << " active threads)" << frigg::endLog;
	}
	if(logNextBest && !_waitQueue.empty())
		frigg::infoLogger() << "    Next entity has priority: " << _waitQueue.top()->priority
				<< ", unfairness: " << (_liveUnfairness(_waitQueue.top()) / 256) / (1000 * 1000)
				<< " ms, runtime: " << _liveRuntime(_waitQueue.top()) / (1000 * 1000)
				<< " ms" << frigg::endLog;

	_current = entity;
}

void Scheduler::_updateSystemProgress() {
	// Returns the reciprocal in 0.8 fixed point format.
	auto fixedInverse = [] (uint32_t x) -> uint32_t {
		assert(x < (1 << 6));
		return static_cast<uint32_t>(1 << 8) / x;
	};

	// Number of waiting/running threads.
	auto n = _numWaiting;
	if(_current)
		n++;

	assert(haveTimer());
	auto now = systemClockSource()->currentNanos();
	auto delta_time = now - _refClock;
	_refClock = now;
	if(n)
		_systemProgress += delta_time * fixedInverse(n);
}

// TODO: Integrate this function and the _refreshFlag() function.
void Scheduler::_updatePreemption() {
	// It does not make sense to preempt if there is no active thread.
	if(!_current || _current->state != ScheduleState::active)
		return; // Hope for thread switch.

	if(_waitQueue.empty()) {
		disarmPreemption();
		return;
	}

	if(auto po = ScheduleEntity::orderPriority(_current, _waitQueue.top()); po > 0) {
		return; // Hope for thread switch.
	}else if(po < 0) {
		// Disable preemption if we have higher priority.
		disarmPreemption();
		return;
	}

	auto diff = _liveUnfairness(_current) - _liveUnfairness(_waitQueue.top());
	if(diff < 0)
		return; // Hope for thread switch.

	auto slice = frigg::max(diff / 256, sliceGranularity);
	if(logTimeSlice)
		frigg::infoLogger() << "Scheduling time slice: "
				<< slice / 1000 << " us" << frigg::endLog;
	armPreemption(slice);
}

void Scheduler::_updateCurrentEntity() {
	assert(_current);

	auto delta_progress = _systemProgress - _current->refProgress;
	if(logUpdates)
		frigg::infoLogger() << "Running thread unfairness decreases by: "
				<< ((_numWaiting * delta_progress) / 256) / 1000
				<< " us (" << _numWaiting << " waiting threads)" << frigg::endLog;
	_current->baseUnfairness -= _numWaiting * delta_progress;
	_current->refProgress = _systemProgress;
}

void Scheduler::_updateWaitingEntity(ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::active);
	assert(entity != _current);

	if(logUpdates)
		frigg::infoLogger() << "Waiting thread unfairness increases by: "
				<< ((_systemProgress - entity->refProgress) / 256) / 1000
				<< " us (" << _numWaiting << " waiting threads)" << frigg::endLog;
	entity->baseUnfairness += _systemProgress - entity->refProgress;
	entity->refProgress = _systemProgress;
}

void Scheduler::_updateEntityStats(ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::active
			|| entity == _current);
	
	if(entity == _current)
		entity->_runTime += _refClock - entity->_refClock;
	entity->_refClock = _refClock;
}

void Scheduler::_refreshFlag() {
	if(_waitQueue.empty()) {
		_scheduleFlag = false;
		return;
	}

	if(_current && _current->state == ScheduleState::active) {
		// Update the unfairness so that scheduleBefore() is correct.
		_updateCurrentEntity();

		if(auto po = ScheduleEntity::orderPriority(_current, _waitQueue.top()); po) {
			_scheduleFlag = po > 0;
			return;
		}

		if(_refClock - _sliceClock < sliceGranularity
				|| ScheduleEntity::scheduleBefore(_current, _waitQueue.top())) {
			_scheduleFlag = false;
			return;
		}
	}

	_scheduleFlag = true;
}

Scheduler *localScheduler() {
	return &getCpuData()->scheduler;
}

frigg::UnsafePtr<Thread> getCurrentThread() {
	return activeExecutor();
}

} // namespace thor

