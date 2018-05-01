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

struct ScheduleEntity {
	static bool scheduleBefore(const ScheduleEntity *a, const ScheduleEntity *b);

	ScheduleEntity();

	ScheduleEntity(const ScheduleEntity &) = delete;

	~ScheduleEntity();
	
	ScheduleEntity &operator= (const ScheduleEntity &) = delete;

	[[ noreturn ]] virtual void invoke() = 0;

	ScheduleState state;
	int priority;

	uint64_t _baseTime;
	uint64_t _runTime;

	frg::pairing_heap_hook<ScheduleEntity> hook;
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

	bool wantSchedule();

	[[ noreturn ]] void reschedule();

private:
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

	size_t _numActive;
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
