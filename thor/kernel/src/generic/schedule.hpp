
namespace thor {

typedef frigg::TicketLock ScheduleLock;
typedef frigg::LockGuard<ScheduleLock> ScheduleGuard;

typedef frigg::LinkedList<KernelUnsafePtr<Thread>, KernelAlloc> ScheduleQueue;
extern frigg::LazyInitializer<ScheduleQueue> scheduleQueue;

extern frigg::LazyInitializer<ScheduleLock> scheduleLock;

KernelUnsafePtr<Thread> getCurrentThread();

// selects an active thread and enters it on this processor
// must only be called if enterThread() would also be allowed
void doSchedule(ScheduleGuard &&guard) __attribute__ (( noreturn ));

void enqueueInSchedule(ScheduleGuard &guard, KernelUnsafePtr<Thread> thread);

// FIXME: do we still use this?
void idleRoutine();

} // namespace thor

