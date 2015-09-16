
namespace thor {

typedef frigg::TicketLock ScheduleLock;
typedef frigg::LockGuard<ScheduleLock> ScheduleGuard;

extern frigg::util::LazyInitializer<ThreadQueue> activeList;

typedef frigg::util::LinkedList<KernelUnsafePtr<Thread>, KernelAlloc> ScheduleQueue;
extern frigg::util::LazyInitializer<ScheduleQueue> scheduleQueue;

extern frigg::util::LazyInitializer<ScheduleLock> scheduleLock;

KernelUnsafePtr<Thread> getCurrentThread();

// resets the current thread on this processor to null
// do not use this function to exit the current thread!
void resetCurrentThread();

// resets the current thread and schedules.
// removes the current thread from the activeList
// use this in conjunction with callOnCpuStack()
void dropCurrentThread() __attribute__ (( noreturn ));

// enters a new thread on this processor
// must only be called if there is no current thread
void enterThread(KernelUnsafePtr<Thread> thread)
		__attribute__ (( noreturn ));

// selects an active thread and enters it on this processor
// must only be called if enterThread() would also be allowed
void doSchedule(ScheduleGuard &&guard) __attribute__ (( noreturn ));

void enqueueInSchedule(ScheduleGuard &guard, KernelUnsafePtr<Thread> thread);

void idleRoutine();

} // namespace thor

