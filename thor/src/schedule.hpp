
namespace thor {

extern LazyInitializer<ThreadQueue> scheduleQueue;

void doSchedule();

void enqueueInSchedule(SharedPtr<Thread, KernelAlloc> &&thread);

} // namespace thor

