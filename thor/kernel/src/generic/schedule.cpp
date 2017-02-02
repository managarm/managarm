
#include "kernel.hpp"

namespace thor {

ScheduleEntity::ScheduleEntity()
: state{ScheduleState::null} { }

ScheduleEntity::~ScheduleEntity() {
	assert(state == ScheduleState::null);
}

Scheduler::Scheduler()
: _scheduleFlag{false}, _current{nullptr} { }

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
	_waitQueue.push_back(entity);

	_refreshFlag();
}

void Scheduler::suspend(ScheduleEntity *entity) {
//	frigg::infoLogger() << "suspend " << entity << frigg::endLog;
	assert(entity->state == ScheduleState::active);
	if(entity != _current)
		_waitQueue.erase(_waitQueue.iterator_to(entity));
	entity->state = ScheduleState::attached;
	
	_refreshFlag();
}

bool Scheduler::wantSchedule() {
	return _scheduleFlag;
}

void Scheduler::reschedule() {
	if(_current && _current->state == ScheduleState::active) {
		_waitQueue.push_back(_current);
		_current = nullptr;
	}

	if(_waitQueue.empty()) {
		suspendSelf();
		frigg::panicLogger() << "Return from suspendSelf()" << frigg::endLog;
	}

	assert(!_waitQueue.empty());
	auto entity = _waitQueue.pop_front();
	assert(entity->state == ScheduleState::active);
	_current = entity;

	_refreshFlag();

	entity->invoke();
	frigg::panicLogger() << "Return from ScheduleEntity::invoke()" << frigg::endLog;
	__builtin_unreachable();
}

void Scheduler::_refreshFlag() {
	_scheduleFlag = !_waitQueue.empty();
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

KernelUnsafePtr<Thread> getCurrentThread() {
	return activeExecutor();
}

} // namespace thor

