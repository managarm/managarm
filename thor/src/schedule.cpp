
#include "kernel.hpp"

namespace traits = frigg::traits;

namespace thor {

LazyInitializer<ThreadQueue> scheduleQueue;

void schedule() {
	ASSERT(!scheduleQueue->empty());
	
	SharedPtr<Thread, KernelAlloc> thread_ptr = scheduleQueue->removeFront();
	switchThread(thread_ptr);

	scheduleQueue->addBack(traits::move(thread_ptr));
	
	thorRtFullReturn();
}

} // namespace thor

