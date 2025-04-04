#pragma once

#include <stddef.h>
#include <stdint.h>

#include <frg/list.hpp>
#include <frg/pairing_heap.hpp>
#include <frg/spinlock.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/arch-generic/cpu.hpp>

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

// Type for system progress and unfairness.
// In units of fractional nanoseconds. We store this as a fixed point number.
using Progress = __int128;

// Number of fractional bits in Progress.
// Note that this must be <= 62 such that (1 << progressShift) is int64_t range.
constexpr int progressShift = 62;

// Convert Progress to whole nanoseconds.
inline int64_t progressToNanos(Progress p) {
	return static_cast<int64_t>(p >> progressShift);
}

struct ScheduleEntity {
	friend struct Scheduler;

	static int orderPriority(const ScheduleEntity *a, const ScheduleEntity *b);
	static bool scheduleBefore(const ScheduleEntity *a, const ScheduleEntity *b);

	ScheduleEntity(ScheduleType type = ScheduleType::regular);

	ScheduleEntity(const ScheduleEntity &) = delete;

	ScheduleEntity &operator= (const ScheduleEntity &) = delete;

protected:
	~ScheduleEntity();

public:
	ScheduleType type() const {
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
	// This function *must* be called in IRQ/fault/syscall exit paths
	// if the IRQ/fault/syscall handler may have woken up threads.
	// Note that this includes timer interrupts and IPIs.
	//
	// In particular, we need to check for preemption (e.g., due to a change in priority)
	// and/or renew the schedule (e.g., if the length of the time slice has changed).
	// See Scheduler::resume() for details.
	//
	// If this function returns true, IRQs/faults/syscalls *must* call into handlePreemption().
	bool mustCallPreemption() {
		return _mustCallPreemption;
	}

	// Force mustCallPreemption() to return true.
	// For example, this is useful to implement the preemption IRQ.
	void forcePreemptionCall() {
		_mustCallPreemption = true;
	}

	// Suppress mustCallPreemption() if a scheduling interrupt is pending.
	// This avoids unnecessary calls into checkPreemption().
	void suppressRenewalUntilInterrupt();

	void checkPreemption(IrqImageAccessor image) {
		assert(image.inPreemptibleDomain());
		if (mustCallPreemption())
			currentRunnable()->handlePreemption(image);
	}

	void update();
	void updateState();
	void updateQueue();

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

	ScheduleEntity *_current;
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

	// See mustCallPreemption().
	bool _mustCallPreemption{false};

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

// Similar to Scheduler::checkPreemption() but specialized for threads.
void checkThreadPreemption(FaultImageAccessor image);
void checkThreadPreemption(SyscallImageAccessor image);

extern PerCpu<Scheduler> localScheduler;

} // namespace thor
