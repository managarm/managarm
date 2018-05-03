
#include "kernel.hpp"

namespace thor {

constexpr bool logScheduling = false;
constexpr bool logNextBest = false;
constexpr bool logUpdates = false;

bool ScheduleEntity::scheduleBefore(const ScheduleEntity *a, const ScheduleEntity *b) {
	if(a->priority != b->priority)
		return a->priority > b->priority; // Prefer larger priority.
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
	assert(entity->state == ScheduleState::null);
	entity->_scheduler = scheduler;
	entity->state = ScheduleState::attached;
}

void Scheduler::unassociate(ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::attached);
	entity->_scheduler = nullptr;
	entity->state = ScheduleState::null;
}

void Scheduler::setPriority(ScheduleEntity *entity, int priority) {
	// Otherwise, we would have to remove-reinsert into the queue.
	auto self = entity->_scheduler;
	assert(self);
	assert(entity == self->_current);

	entity->priority = priority;
}

void Scheduler::resume(ScheduleEntity *entity) {
//	frigg::infoLogger() << "resume " << entity << frigg::endLog;
	assert(entity->state == ScheduleState::attached);

	auto self = entity->_scheduler;
	assert(self);
	assert(entity != self->_current); // TODO: The other case is untested.

	self->_updateSystemProgress();

	// Update the unfairness reference on resume.
	if(self->_current)
		self->_updateCurrentEntity();
	if(entity != self->_current) {
		entity->refProgress = self->_systemProgress;
		entity->_refClock = self->_refClock;
	}
	entity->state = ScheduleState::active;
	
	if(entity != self->_current) {
		self->_waitQueue.push(entity);
		self->_numWaiting++;
	}
}

void Scheduler::suspend(ScheduleEntity *entity) {
//	frigg::infoLogger() << "suspend " << entity << frigg::endLog;
	assert(entity->state == ScheduleState::active);
	
	auto self = entity->_scheduler;
	assert(self);
	assert(entity == self->_current); // TODO: The other case is untested.
	
	self->_updateSystemProgress();

	// Update the unfairness on suspend.
	if(self->_current)
		self->_updateCurrentEntity();
	if(entity != self->_current) {
		self->_updateWaitingEntity(entity);
		self->_updateEntityStats(entity);
	}
	entity->state = ScheduleState::attached;

	if(entity != self->_current) {
		self->_waitQueue.remove(entity); // TODO: Pairing heap remove() is untested.
		self->_numWaiting--;
	}
}

Scheduler::Scheduler()
: _scheduleFlag{false}, _current{nullptr},
		_numWaiting{0}, _refClock{0}, _systemProgress{0} { }

Progress Scheduler::liveUnfairness(const ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::active);

	auto delta_progress = _systemProgress - entity->refProgress;
	if(entity == _current) {
		return entity->baseUnfairness - _numWaiting * delta_progress;
	}else{
		return entity->baseUnfairness + delta_progress;
	}
}

int64_t Scheduler::liveRuntime(const ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::active);
	if(entity == _current) {
		return entity->_runTime + (_refClock - entity->_refClock);
	}else{
		return entity->_runTime;
	}
}

bool Scheduler::wantSchedule() {
	_updateSystemProgress();
	_refreshFlag();
	return _scheduleFlag;
}

void Scheduler::reschedule() {
	_updateSystemProgress();

	if(_current)
		_unschedule();
	
	if(_waitQueue.empty()) {
		if(logScheduling)
			frigg::infoLogger() << "System is idle" << frigg::endLog;
		suspendSelf();
		frigg::panicLogger() << "Return from suspendSelf()" << frigg::endLog;
	}

	_schedule();
	assert(_current);

	if(!_waitQueue.empty()) {
		// TODO: Impose a minimum slice length.
		// TODO: Only preempt if the priorities are the same.
		auto slice = liveUnfairness(_current) - liveUnfairness(_waitQueue.top());
		assert(slice >= 0);
		preemptThisCpu(slice);
	}

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
				<< ", unfairness: " << (liveUnfairness(entity) / 256) / (1000 * 1000)
				<< " ms, runtime: " << liveRuntime(entity) / (1000 * 1000)
				<< " ms (" << (_numWaiting + 1) << " active threads)" << frigg::endLog;
	}
	if(logNextBest && !_waitQueue.empty())
		frigg::infoLogger() << "    Next entity has priority: " << _waitQueue.top()->priority
				<< ", unfairness: " << (liveUnfairness(_waitQueue.top()) / 256) / (1000 * 1000)
				<< " ms, runtime: " << liveRuntime(_waitQueue.top()) / (1000 * 1000)
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
	auto now = currentNanos();
	auto delta_time = now - _refClock;
	_refClock = now;
	if(n)
		_systemProgress += delta_time * fixedInverse(n);
}

void Scheduler::_updateCurrentEntity() {
	assert(_current);

	auto delta_progress = _systemProgress - _current->refProgress;
	if(logUpdates)
		frigg::infoLogger() << "Running thread unfairness decreases by: "
				<< ((_numWaiting * delta_progress) / 256) / (1000)
				<< " us (" << _numWaiting << " waiting threads)" << frigg::endLog;
	_current->baseUnfairness -= _numWaiting * delta_progress;
	_current->refProgress = _systemProgress;
}

void Scheduler::_updateWaitingEntity(ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::active);
	assert(entity != _current);

	if(logUpdates)
		frigg::infoLogger() << "Waiting thread unfairness increases by: "
				<< ((_systemProgress - entity->refProgress) / 256) / (1000)
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

		if(ScheduleEntity::scheduleBefore(_current, _waitQueue.top())) {
			_scheduleFlag = false;
			return;
		}
	}

	_scheduleFlag = true;
}

// ----------------------------------------------------------------------------

void WorkQueue::post(Tasklet *tasklet) {
	if(_queue.empty())
		Scheduler::resume(this);

	_queue.push_back(tasklet);
}

void WorkQueue::invoke() {
	while(!_queue.empty()) {
		auto tasklet = _queue.pop_front();

		// We suspend the WorkQueue before invoking the tasklet. This way we do not call
		// resume() before suspend() if the last tasklet inserts another tasklet to this queue.
		if(_queue.empty())
			Scheduler::suspend(this);

		tasklet->run();
	}

	// Note that we are currently running in the schedule context. Thus runDetached() trashes
	// our own stack. We need to be careful not to access it in the callback.
	runDetached([] {
		localScheduler()->reschedule();
	});
}

frigg::LazyInitializer<WorkQueue> workQueueSingleton;

WorkQueue &globalWorkQueue() {
	if(!workQueueSingleton) {
		workQueueSingleton.initialize();
		Scheduler::associate(workQueueSingleton.get(), localScheduler());
	}
	return *workQueueSingleton;
}

Scheduler *localScheduler() {
	return &getCpuData()->scheduler;
}

frigg::UnsafePtr<Thread> getCurrentThread() {
	return activeExecutor();
}

} // namespace thor

