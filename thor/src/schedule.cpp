
#include "../../frigg/include/arch_x86/types64.hpp"
#include "util/general.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "util/vector.hpp"
#include "util/smart-ptr.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"
#include "core.hpp"
#include "schedule.hpp"

namespace thor {

LazyInitializer<util::LinkedList<SharedPtr<Thread>, KernelAlloc>> scheduleQueue;

void schedule() {
	if(scheduleQueue->empty()) {
		debug::criticalLogger->log("No threads to schedule!");
		debug::panic();
	}
	
	SharedPtr<Thread> thread_ptr = scheduleQueue->removeFront();
	thread_ptr->switchTo();
	debug::criticalLogger->log(thread_ptr.get());

	scheduleQueue->addBack(util::move(thread_ptr));
	
	thorRtFullReturn();
}

} // namespace thor

