
#include "kernel.hpp"

namespace thor {

// TODO: move this to a header file
extern frigg::LazyInitializer<frigg::SharedPtr<Universe>> rootUniverse;

void serviceMain() {
	//frigg::infoLogger.log() << "In service thread" << frigg::EndLog();
	while(true) { }
}

void runService() {
/*	auto space = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());
	space->setupDefaultMappings();

	// allocate and map memory for the user mode stack
	size_t stack_size = 0x10000;
	auto stack_memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeAllocated);
	stack_memory->resize(stack_size / kPageSize);

	VirtualAddr stack_base;
	{
		AddressSpace::Guard space_guard(&space->lock);
		space->map(space_guard, stack_memory, 0, 0, stack_size,
				AddressSpace::kMapPreferTop | AddressSpace::kMapReadWrite, &stack_base);
	}

	// create a thread for the module
	auto thread = frigg::makeShared<Thread>(*kernelAlloc, frigg::SharedPtr<Universe>(),
			frigg::move(space), frigg::SharedPtr<RdFolder>());
	thread->flags |= Thread::kFlagExclusive | Thread::kFlagTrapsAreFatal;
	
	*thread->image.sp() = stack_base + stack_size;
	*thread->image.ip() = (uintptr_t)&serviceMain;
	*thread->image.kernel() = 1;

	// increment the reference counter so that the threads stays alive forever
	thread.control().increment();

	ScheduleGuard schedule_guard(scheduleLock.get());
	enqueueInSchedule(schedule_guard, frigg::move(thread));*/
}

}

