
#include "kernel.hpp"

namespace traits = frigg::traits;

namespace thor {

LazyInitializer<ThreadQueue> scheduleQueue;

void doSchedule() {
	currentThread->reset();
	
	ASSERT(!scheduleQueue->empty());
	SharedPtr<Thread, KernelAlloc> thread = scheduleQueue->removeFront();
	switchThread(thread);
	
	if(!(*currentThread)->isKernelThread()) {
		thorRtFullReturn();
	}else{
		thorRtFullReturnToKernel();
	}
}

void enqueueInSchedule(SharedPtr<Thread, KernelAlloc> &&thread) {
	scheduleQueue->addBack(traits::move(thread));
}

} // namespace thor

