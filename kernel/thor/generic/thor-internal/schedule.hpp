#pragma once

#include <stddef.h>
#include <stdint.h>

#include <frg/list.hpp>
#include <frg/pairing_heap.hpp>
#include <frg/spinlock.hpp>

namespace thor {

struct Scheduler;
struct CpuData;

enum class ScheduleType {
	none,
	idle,
	regular
};

enum class ScheduleState {
	null,
	attached,
	pending,
	active
};

// This needs to store a large timeframe.
// For now, store it as 55.8 0 signed integer nanoseconds.
using Progress = int64_t;

struct ScheduleEntity {
	friend struct Scheduler;

	static int orderPriority(const ScheduleEntity *a, const ScheduleEntity *b);
	static bool scheduleBefore(const ScheduleEntity *a, const ScheduleEntity *b);

	ScheduleEntity(ScheduleType type = ScheduleType::idle);

	ScheduleEntity(const ScheduleEntity &) = delete;

	ScheduleEntity &operator= (const ScheduleEntity &) = delete;

protected:
	~ScheduleEntity();

public:
	ScheduleType type() {
		return type_;
	}

	[[ noreturn ]] virtual void invoke() = 0;

	virtual void handlePreemption(IrqImageAccessor image) = 0;

	uint64_t runTime() {
		return _runTime;
	}

private:
	const ScheduleType type_;

	frg::ticket_spinlock _associationMutex;
	Scheduler *_scheduler;

	ScheduleState state;
	int priority;

	frg::default_list_hook<ScheduleEntity> listHook;
	frg::pairing_heap_hook<ScheduleEntity> heapHook;

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
			return po > 0;
		return !ScheduleEntity::scheduleBefore(a, b);
	}
};

struct Scheduler {
	// Note: the scheduler's methods (e.g., associate, unassociate, resume, ...)
	// may be called from any CPU, *however*, calling them on the same ScheduleEntity is
	// *not* thread-safe without additional synchronization!

	static void associate(ScheduleEntity *entity, Scheduler *scheduler);
	static void unassociate(ScheduleEntity *entity);

	static void setPriority(ScheduleEntity *entity, int priority);

	static void resume(ScheduleEntity *entity);
	static void suspendCurrent();

	Scheduler(CpuData *cpu_context);

	Scheduler(const Scheduler &) = delete;

	Scheduler &operator= (const Scheduler &) = delete;

private:
	Progress _liveUnfairness(const ScheduleEntity *entity);
	int64_t _liveRuntime(const ScheduleEntity *entity);

public:
	void update();
	void forcePreemptionUpdate();

	bool maybeReschedule();
	void forceReschedule();

	[[noreturn]] void commitReschedule();
	void renewSchedule();

	ScheduleEntity *currentRunnable();

private:
	void _unschedule();
	void _schedule();

private:
	void _updatePreemption();

	void _updateCurrentEntity();
	void _updateWaitingEntity(ScheduleEntity *entity);

	void _updateEntityStats(ScheduleEntity *entity);

	CpuData *_cpuContext;

	ScheduleEntity *_current = nullptr;
	ScheduleEntity *_scheduled = nullptr;

	frg::pairing_heap<
		ScheduleEntity,
		frg::locate_member<
			ScheduleEntity,
			frg::pairing_heap_hook<ScheduleEntity>,
			&ScheduleEntity::heapHook
		>,
		ScheduleGreater
	> _waitQueue;

	size_t _numWaiting = 0;

	// The last tick at which the scheduler's state (i.e. progress) was updated.
	// In our model this is the time point at which slice T started.
	uint64_t _refClock = 0;

	// Start of the current timeslice.
	uint64_t _sliceClock;

	// This variables stores sum{t = 0, ... T} w(t)/n(t).
	// This allows us to easily track u_p(T) for all waiting processes.
	Progress _systemProgress = 0;

	// ----------------------------------------------------------------------------------
	// Management of pending entities.
	// ----------------------------------------------------------------------------------

	// Note that _mutex *only* protects _pendingList and nothing more!
	frg::ticket_spinlock _mutex;

	frg::intrusive_list<
		ScheduleEntity,
		frg::locate_member<
			ScheduleEntity,
			frg::default_list_hook<ScheduleEntity>,
			&ScheduleEntity::listHook
		>
	> _pendingList;
};

Scheduler *localScheduler();

} // namespace thor
