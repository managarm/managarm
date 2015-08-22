
namespace thor {

extern frigg::util::LazyInitializer<ThreadQueue> scheduleQueue;

KernelUnsafePtr<Thread> getCurrentThread();

// resets the current thread on this processor to null
// do not use this function to exit the current thread!
KernelSharedPtr<Thread> resetCurrentThread();

// resets the current thread and schedule.
// use this in conjunction with callOnCpuStack()
void dropCurrentThread() __attribute__ (( noreturn ));

// enters a new thread on this processor
// must only be called if there is no current thread
void enterThread(KernelSharedPtr<Thread> &&thread)
		__attribute__ (( noreturn ));

// selects an active thread and enters it on this processor
// must only be called if enterThread() would also be allowed
void doSchedule() __attribute__ (( noreturn ));

void enqueueInSchedule(KernelSharedPtr<Thread> &&thread);

} // namespace thor

