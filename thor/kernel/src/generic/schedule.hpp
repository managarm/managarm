#ifndef THOR_GENERIC_SCHEDULE_HPP
#define THOR_GENERIC_SCHEDULE_HPP

#include <frg/list.hpp>
#include <frg/pairing_heap.hpp>

namespace thor {

struct Scheduler;

enum class ScheduleState {
	null,
	attached,
	active
};

// This needs to store a large timeframe.
// For now, store it as 55.8 0 signed integer nanoseconds.
using Progress = int64_t;

struct ScheduleEntity {
	friend struct Scheduler;

	static int orderPriority(const ScheduleEntity *a, const ScheduleEntity *b);
	static bool scheduleBefore(const ScheduleEntity *a, const ScheduleEntity *b);

	ScheduleEntity();

	ScheduleEntity(const ScheduleEntity &) = delete;

	~ScheduleEntity();
	
	ScheduleEntity &operator= (const ScheduleEntity &) = delete;

	uint64_t runTime() {
		return _runTime;
	}

	[[ noreturn ]] virtual void invoke() = 0;

private:
	frigg::TicketLock _associationMutex;
	Scheduler *_scheduler;

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
		if(int po = ScheduleEntity::orderPriority(a, b); po)
			return -po;
		return !ScheduleEntity::scheduleBefore(a, b);
	}
};

struct Scheduler {
	static void associate(ScheduleEntity *entity, Scheduler *scheduler);
	static void unassociate(ScheduleEntity *entity);

	static void setPriority(ScheduleEntity *entity, int priority);

	static void resume(ScheduleEntity *entity);
	static void suspendCurrent();
	static void suspendWaiting(ScheduleEntity *entity);

	Scheduler(CpuData *cpu_context);

	Scheduler(const Scheduler &) = delete;

	Scheduler &operator= (const Scheduler &) = delete;

private:
	Progress _liveUnfairness(const ScheduleEntity *entity);
	int64_t _liveRuntime(const ScheduleEntity *entity);

public:
	bool wantSchedule();

	[[ noreturn ]] void reschedule();

private:
	void _unschedule();
	void _schedule();

private:
	void _updateSystemProgress();
	void _updatePreemption();

	void _updateCurrentEntity();
	void _updateWaitingEntity(ScheduleEntity *entity);

	void _updateEntityStats(ScheduleEntity *entity);

	CpuData *_cpuContext;

	frigg::TicketLock _mutex;

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

	// Start of the current timeslice.
	uint64_t _sliceClock;

	// This variables stores sum{t = 0, ... T} w(t)/n(t).
	// This allows us to easily track u_p(T) for all waiting processes.
	Progress _systemProgress;
};

Scheduler *localScheduler();

} // namespace thor

#endif // THOR_GENERIC_SCHEDULE_HPP
