
#include "util/linked.hpp"

namespace thor {

extern LazyInitializer<util::LinkedList<SharedPtr<Thread>, KernelAlloc>> scheduleQueue;

void schedule();

} // namespace thor

