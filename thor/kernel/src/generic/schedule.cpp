
#include "kernel.hpp"

namespace thor {

constexpr bool logScheduling = false;

bool ScheduleEntity::scheduleBefore(const ScheduleEntity *a, const ScheduleEntity *b) {
//	if(a->priority != b->priority)
//		return a->priority > b->priority; // Prefer larger priority.
	return a->_runTime < b->_runTime; // Prefer smaller runtime.
}

ScheduleEntity::ScheduleEntity()
: state{ScheduleState::null}, priority(0), _baseTime{0}, _runTime{0} { }

ScheduleEntity::~ScheduleEntity() {
	assert(state == ScheduleState::null);
}

Scheduler::Scheduler()
: _scheduleFlag{false}, _current{nullptr}, _numActive{0} { }

void Scheduler::attach(ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::null);
	entity->state = ScheduleState::attached;
}

void Scheduler::detach(ScheduleEntity *entity) {
	assert(entity->state == ScheduleState::attached);
	entity->state = ScheduleState::null;
}

void Scheduler::resume(ScheduleEntity *entity) {
//	frigg::infoLogger() << "resume " << entity << frigg::endLog;
	assert(entity->state == ScheduleState::attached);
	entity->state = ScheduleState::active;
	_waitQueue.push(entity);
	_numActive++;
}

void Scheduler::suspend(ScheduleEntity *entity) {
//	frigg::infoLogger() << "suspend " << entity << frigg::endLog;
	assert(entity->state == ScheduleState::active);
	if(entity != _current) {
		frigg::infoLogger() << "Entering remove" << frigg::endLog;
		_waitQueue.remove(entity);
		frigg::infoLogger() << "OK" << frigg::endLog;
	}
	entity->state = ScheduleState::attached;
	_numActive--;
}

void Scheduler::setPriority(ScheduleEntity *entity, int priority) {
	// Otherwise, we would have to remove-reinsert into the queue.
	assert(_current == entity);

	entity->priority = priority;
}

bool Scheduler::wantSchedule() {
	_refreshFlag();
	return _scheduleFlag;
}

void Scheduler::reschedule() {
	assert(haveTimer());
	auto now = currentNanos();

	if(_current) {
		_current->_runTime += now - _current->_baseTime;

		if(_current->state == ScheduleState::active)
			_waitQueue.push(_current);
		_current = nullptr;
	}

	if(_waitQueue.empty()) {
		suspendSelf();
		frigg::panicLogger() << "Return from suspendSelf()" << frigg::endLog;
	}

	assert(!_waitQueue.empty());
	auto entity = _waitQueue.top();
	_waitQueue.pop();
	assert(entity->state == ScheduleState::active);

	if(logScheduling)
		frigg::infoLogger() << "Running entity with priority: " << entity->priority
				<< ", run time: " << entity->_runTime
				<< " (" << _numActive << " active threads)" << frigg::endLog;
//	if(!_waitQueue.empty())
//		frigg::infoLogger() << "    New top has priority: " << _waitQueue.top()->priority
//				<< ", run time: " << _waitQueue.top()->runTime << frigg::endLog;

	entity->_baseTime = now;
	_current = entity;
	_scheduleFlag = false;

	entity->invoke();
	frigg::panicLogger() << "Return from ScheduleEntity::invoke()" << frigg::endLog;
	__builtin_unreachable();
}

void Scheduler::_refreshFlag() {
	if(_waitQueue.empty()) {
		_scheduleFlag = false;
		return;
	}

	if(_current) {
		assert(haveTimer());
		auto now = currentNanos();
		_current->_runTime += now - _current->_baseTime;
		_current->_baseTime = now;

		if(ScheduleEntity::scheduleBefore(_current, _waitQueue.top())) {
			_scheduleFlag = false;
			return;
		}
	}

	_scheduleFlag = true;
}

frigg::LazyInitializer<Scheduler> schedulerSingleton;

Scheduler &globalScheduler() {
	if(!schedulerSingleton)
		schedulerSingleton.initialize();
	return *schedulerSingleton;
}

// ----------------------------------------------------------------------------

void WorkQueue::post(Tasklet *tasklet) {
	if(_queue.empty())
		globalScheduler().resume(this);

	_queue.push_back(tasklet);
}

void WorkQueue::invoke() {
	while(!_queue.empty()) {
		auto tasklet = _queue.pop_front();

		// We suspend the WorkQueue before invoking the tasklet. This way we do not call
		// resume() before suspend() if the last tasklet inserts another tasklet to this queue.
		if(_queue.empty())
			globalScheduler().suspend(this);

		tasklet->run();
	}

	// Note that we are currently running in the schedule context. Thus runDetached() trashes
	// our own stack. We need to be careful not to access it in the callback.
	runDetached([] {
		globalScheduler().reschedule();
	});
}

frigg::LazyInitializer<WorkQueue> workQueueSingleton;

WorkQueue &globalWorkQueue() {
	if(!workQueueSingleton) {
		workQueueSingleton.initialize();
		globalScheduler().attach(workQueueSingleton.get());
	}
	return *workQueueSingleton;
}

frigg::UnsafePtr<Thread> getCurrentThread() {
	return activeExecutor();
}

} // namespace thor

