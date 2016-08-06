
#include "kernel.hpp"

namespace thor {

// TODO: move this to a header file
extern frigg::LazyInitializer<frigg::SharedPtr<Universe>> rootUniverse;

void serviceMain() {
	frigg::UnsafePtr<Thread> this_thread = getCurrentThread();

	frigg::infoLogger() << "In service thread" << frigg::endLog;

	while(true) {
		disableInts();
		if(forkExecutor()) {
			ScheduleGuard schedule_guard(scheduleLock.get());
			enqueueInSchedule(schedule_guard, this_thread.toShared());
			doSchedule(frigg::move(schedule_guard));
		}
		enableInts();
	}
}

void runService() {
	// FIXME: reactive this after memory is reworked
	return;

	auto space = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());
	space->setupDefaultMappings();

	// allocate and map memory for the user mode stack
	size_t stack_size = 0x10000;
	auto stack_memory = frigg::makeShared<Memory>(*kernelAlloc,
			AllocatedMemory(stack_size));
	assert(!"Pre-populate the stack to prevent us entering an infinite loop of page faults");

	{
		assert(!"Fix setPageAt()");
/*		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		for(size_t i = 0; i < stack_memory->numPages(); i++)
			stack_memory->setPageAt(i * kPageSize,
					physicalAllocator->allocate(physical_guard, kPageSize));*/
	}

	VirtualAddr stack_base;
	{
		AddressSpace::Guard space_guard(&space->lock);
		space->map(space_guard, stack_memory, 0, 0, stack_size,
				AddressSpace::kMapPreferTop | AddressSpace::kMapReadWrite, &stack_base);
	}
	thorRtInvalidateSpace();

	// create a thread for the module
	auto thread = frigg::makeShared<Thread>(*kernelAlloc, frigg::SharedPtr<Universe>(),
			frigg::move(space), frigg::SharedPtr<RdFolder>());
	thread->flags |= Thread::kFlagExclusive | Thread::kFlagTrapsAreFatal;
	
	thread->image.initSystemVAbi((uintptr_t)&serviceMain,
//			stack_base + stack_size, true);
			(uintptr_t)thread->kernelStack.base(), true);

	// see helCreateThread for the reasoning here
	thread.control().increment();
	thread.control().increment();

//	ScheduleGuard schedule_guard(scheduleLock.get());
//	enqueueInSchedule(schedule_guard, frigg::move(thread));
}

} // namespace thor

