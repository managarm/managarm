
namespace thor {

extern frigg::util::LazyInitializer<ThreadQueue> scheduleQueue;

// resets the current thread on this processor to null
SharedPtr<Thread, KernelAlloc> resetCurrentThread();

// enters a new thread on this processor
// must only be called if there is no current thread
void enterThread(UnsafePtr<Thread, KernelAlloc> thread);

// selects an active thread and enters it on this processor
// must only be called if enterThread() would also be allowed
void doSchedule() __attribute__ (( noreturn ));

void enqueueInSchedule(SharedPtr<Thread, KernelAlloc> &&thread);

} // namespace thor

