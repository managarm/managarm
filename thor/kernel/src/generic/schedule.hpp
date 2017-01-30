
#include <frg/list.hpp>

namespace thor {

enum class ScheduleState {
	null,
	attached,
	active
};

struct ScheduleEntity {
	ScheduleEntity();

	ScheduleEntity(const ScheduleEntity &) = delete;

	~ScheduleEntity();
	
	ScheduleEntity &operator= (const ScheduleEntity &) = delete;

	[[ noreturn ]] virtual void invoke() = 0;

	ScheduleState state;

	frg::default_list_hook<ScheduleEntity> hook;
};

struct Scheduler {
	Scheduler();

	Scheduler(const Scheduler &) = delete;

	~Scheduler() = delete;
	
	Scheduler &operator= (const Scheduler &) = delete;

	void attach(ScheduleEntity *entity);

	void resume(ScheduleEntity *entity);

	void suspend(ScheduleEntity *entity);

	bool wantSchedule();

	[[ noreturn ]] void reschedule();

private:
	ScheduleEntity *_current;

	frg::intrusive_list<
		ScheduleEntity,
		frg::locate_member<
			ScheduleEntity,
			frg::default_list_hook<ScheduleEntity>,
			&ScheduleEntity::hook
		>
	> _waitQueue;
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

