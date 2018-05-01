#ifndef THOR_GENERIC_SCHEDULE_HPP
#define THOR_GENERIC_SCHEDULE_HPP

#include <frg/list.hpp>
#include <frg/pairing_heap.hpp>

namespace thor {

enum class ScheduleState {
	null,
	attached,
	active
};

// This needs to store a large timeframe.
// For now, store it as 55.8 0 signed integer nanoseconds.
using Progress = int64_t;

struct ScheduleEntity {
	static bool scheduleBefore(const ScheduleEntity *a, const ScheduleEntity *b);

	ScheduleEntity();

	ScheduleEntity(const ScheduleEntity &) = delete;

	~ScheduleEntity();
	
	ScheduleEntity &operator= (const ScheduleEntity &) = delete;

	[[ noreturn ]] virtual void invoke() = 0;

	ScheduleState state;
	int priority;
	
	frg::pairing_heap_hook<ScheduleEntity> hook;

	uint64_t _refClock;
	uint64_t _runTime;

	// Scheduler::_systemProgress value at some slice T.
	// Invariant: This entity's state did not change since T.
	Progress refProgress;

	// Unfairness value at slice T.
	Progress baseUnfairness;
};

struct ScheduleGreater {
	bool operator() (const ScheduleEntity *a, const ScheduleEntity *b) {
		return !ScheduleEntity::scheduleBefore(a, b);
	}
};

struct Scheduler {
	Scheduler();

	Scheduler(const Scheduler &) = delete;

	~Scheduler() = delete;
	
	Scheduler &operator= (const Scheduler &) = delete;

	void attach(ScheduleEntity *entity);
	
	void detach(ScheduleEntity *entity);

	void resume(ScheduleEntity *entity);

	void suspend(ScheduleEntity *entity);

	void setPriority(ScheduleEntity *entity, int priority);

	Progress liveUnfairness(const ScheduleEntity *entity);
	int64_t liveRuntime(const ScheduleEntity *entity);

	bool wantSchedule();

	[[ noreturn ]] void reschedule();

private:
	void _unschedule();
	void _schedule();

private:
	void _updateSystemProgress();

	void _updateCurrentEntity();
	void _updateWaitingEntity(ScheduleEntity *entity);

	void _updateEntityStats(ScheduleEntity *entity);

	// Updates the current value of _scheduleFlag.
	void _refreshFlag();

	// This value is returned by wantSchedule().
	bool _scheduleFlag;

	ScheduleEntity *_current;
	
	frg::pairing_heap<
		ScheduleEntity,
		frg::locate_member<
			ScheduleEntity,
			frg::pairing_heap_hook<ScheduleEntity>,
			&ScheduleEntity::hook
		>,
		ScheduleGreater
	> _waitQueue;

	size_t _numWaiting;

	// The last tick at which the scheduler's state (i.e. progress) was updated.
	// In our model this is the time point at which slice T started.
	uint64_t _refClock;

	// This variables stores sum{t = 0, ... T} w(t)/n(t).
	// This allows us to easily track u_p(T) for all waiting processes.
	Progress _systemProgress;
};

// ----------------------------------------------------------------------------

struct Tasklet {
	virtual void run();

	frg::default_list_hook<Tasklet> hook;
};

struct WorkQueue : ScheduleEntity {
	void post(Tasklet *tasklet);

	void invoke() override;

private:
	frg::intrusive_list<
		Tasklet,
		frg::locate_member<
			Tasklet,
			frg::default_list_hook<Tasklet>,
			&Tasklet::hook
		>
	> _queue;
};

WorkQueue &globalWorkQueue();

Scheduler &globalScheduler();

} // namespace thor

#endif // THOR_GENERIC_SCHEDULE_HPP
