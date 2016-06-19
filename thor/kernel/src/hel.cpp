
#include "kernel.hpp"

using namespace thor;

HelError helLog(const char *string, size_t length) {
	for(size_t i = 0; i < length; i++)
		infoSink.print(string[i]);

	return kHelErrNone;
}


HelError helDescriptorInfo(HelHandle handle, HelDescriptorInfo *user_info) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&universe->lock);
	auto wrapper = universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	switch(wrapper->tag()) {
	case AnyDescriptor::tagOf<EndpointDescriptor>():
		user_info->type = kHelDescEndpoint;
		break;
	case AnyDescriptor::tagOf<EventHubDescriptor>():
		user_info->type = kHelDescEventHub;
		break;
	case AnyDescriptor::tagOf<ServerDescriptor>():
		user_info->type = kHelDescServer;
		break;
	case AnyDescriptor::tagOf<ClientDescriptor>():
		user_info->type = kHelDescClient;
		break;
	case AnyDescriptor::tagOf<RdDescriptor>():
		user_info->type = kHelDescDirectory;
		break;
	default:
		assert(!"Illegal descriptor");
	}
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helCloseDescriptor(HelHandle handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&universe->lock);
	if(!universe->detachDescriptor(universe_guard, handle))
		return kHelErrNoDescriptor;
	universe_guard.unlock();

	return kHelErrNone;
}


HelError helAllocateMemory(size_t size, uint32_t flags, HelHandle *handle) {
	assert(size > 0);
	assert(size % kPageSize == 0);

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	frigg::SharedPtr<Memory> memory;
	if(flags & kHelAllocContinuous) {
		memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeAllocated);
		memory->resize(size / kPageSize);

		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		PhysicalAddr address = physicalAllocator->allocate(physical_guard, size);
		physical_guard.unlock();
		
		for(size_t i = 0; i < memory->numPages(); i++)
			memory->setPageAt(i * kPageSize, address + i * kPageSize);
		memory->zeroPages();
	}else if(flags & kHelAllocOnDemand) {
		memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeOnDemand);
		memory->resize(size / kPageSize);
	}else if(flags & kHelAllocBacked) {
		memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeBacked);
		memory->resize(size / kPageSize);
		memory->loadState.resize(size / kPageSize);
	}else{
		memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeAllocated);
		memory->resize(size / kPageSize);

		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		for(size_t i = 0; i < memory->numPages(); i++)
			memory->setPageAt(i * kPageSize,
					physicalAllocator->allocate(physical_guard, kPageSize));
		physical_guard.unlock();

		memory->zeroPages();
	}
	
	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			MemoryAccessDescriptor(frigg::move(memory)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helAccessPhysical(uintptr_t physical, size_t size, HelHandle *handle) {
	assert((physical % kPageSize) == 0);
	assert((size % kPageSize) == 0);

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypePhysical);
	memory->resize(size / kPageSize);
	for(size_t i = 0; i < memory->numPages(); i++)
		memory->setPageAt(i * kPageSize, physical + i * kPageSize);
	
	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			MemoryAccessDescriptor(frigg::move(memory)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helCreateSpace(HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	auto space = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());
	space->setupDefaultMappings();
	
	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			AddressSpaceDescriptor(frigg::move(space)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helForkSpace(HelHandle handle, HelHandle *forked_handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
	{
		Universe::Guard universe_guard(&universe->lock);

		if(handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().toShared();
		}else{
			auto space_wrapper = universe->getDescriptor(universe_guard, handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}

	AddressSpace::Guard space_guard(&space->lock);
	auto forked = space->fork(space_guard);
	space_guard.unlock();
	
	{
		Universe::Guard universe_guard(&universe->lock);

		*forked_handle = universe->attachDescriptor(universe_guard,
				AddressSpaceDescriptor(frigg::move(forked)));
	}

	return kHelErrNone;
}

HelError helMapMemory(HelHandle memory_handle, HelHandle space_handle,
		void *pointer, uintptr_t offset, size_t length, uint32_t flags, void **actual_pointer) {
	if(length == 0)
		return kHelErrIllegalArgs;
	if((uintptr_t)pointer % kPageSize != 0)
		return kHelErrIllegalArgs;
	if(offset % kPageSize != 0)
		return kHelErrIllegalArgs;
	if(length % kPageSize != 0)
		return kHelErrIllegalArgs;

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	frigg::SharedPtr<AddressSpace> space;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto memory_wrapper = universe->getDescriptor(universe_guard, memory_handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
		
		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().toShared();
		}else{
			auto space_wrapper = universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}

	// TODO: check proper alignment

	uint32_t map_flags = 0;
	if(pointer != nullptr) {
		map_flags |= AddressSpace::kMapFixed;
	}else{
		map_flags |= AddressSpace::kMapPreferTop;
	}

	constexpr int mask = kHelMapReadOnly | kHelMapReadWrite | kHelMapReadExecute;
	if((flags & mask) == kHelMapReadWrite) {
		map_flags |= AddressSpace::kMapReadWrite;
	}else if((flags & mask) == kHelMapReadExecute) {
		map_flags |= AddressSpace::kMapReadExecute;
	}else{
		assert((flags & mask) == kHelMapReadOnly);
		map_flags |= AddressSpace::kMapReadOnly;
	}

	if(flags & kHelMapShareOnFork)
		map_flags |= AddressSpace::kMapShareOnFork;
	if(flags & kHelMapBacking)
		map_flags |= AddressSpace::kMapBacking;
	
	VirtualAddr actual_address;
	AddressSpace::Guard space_guard(&space->lock);
	space->map(space_guard, memory, (VirtualAddr)pointer, offset, length,
			map_flags, &actual_address);
	space_guard.unlock();
	
	thorRtInvalidateSpace();

	*actual_pointer = (void *)actual_address;

	return kHelErrNone;
}

HelError helUnmapMemory(HelHandle space_handle, void *pointer, size_t length) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&universe->lock);
	KernelSharedPtr<AddressSpace> space;
	if(space_handle == kHelNullHandle) {
		space = this_thread->getAddressSpace().toShared();
	}else{
		auto space_wrapper = universe->getDescriptor(universe_guard, space_handle);
		if(!space_wrapper)
			return kHelErrNoDescriptor;
		if(!space_wrapper->is<AddressSpaceDescriptor>())
			return kHelErrBadDescriptor;
		space = space_wrapper->get<AddressSpaceDescriptor>().space;
	}
	universe_guard.unlock();
	
	AddressSpace::Guard space_guard(&space->lock);
	space->unmap(space_guard, (VirtualAddr)pointer, length);
	space_guard.unlock();

	return kHelErrNone;
}

HelError helPointerPhysical(void *pointer, uintptr_t *physical) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	
	KernelSharedPtr<AddressSpace> space = this_thread->getAddressSpace().toShared();

	auto address = (VirtualAddr)pointer;
	auto misalign = address % kPageSize;

	AddressSpace::Guard space_guard(&space->lock);
	PhysicalAddr page_physical = space->getPhysical(space_guard, address - misalign);
	space_guard.unlock();

	*physical = page_physical + misalign;

	return kHelErrNone;
}

HelError helMemoryInfo(HelHandle handle, size_t *size) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto wrapper = universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = wrapper->get<MemoryAccessDescriptor>().memory;
	}

	*size = memory->numPages() * kPageSize;
	return kHelErrNone;
}

HelError helSubmitProcessLoad(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);
		auto memory_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
		
		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}

	SubmitInfo submit_info(allocAsyncId(), submit_function, submit_object);
	Memory::ProcessRequest process_request(frigg::move(event_hub), submit_info);

	if(!memory->loadQueue.empty()) {
		Memory::LoadOrder load_order = memory->loadQueue.removeFront();
		memory->performLoad(&process_request, &load_order);
	}else{
		memory->processQueue.addBack(frigg::move(process_request));
	}

	return kHelErrNone;
}

HelError helCompleteLoad(HelHandle handle, uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto memory_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
	}

	// mark the page as loaded
	for(uintptr_t page = 0; page < length; page += kPageSize)
		memory->loadState[(offset + page) / kPageSize] = Memory::kStateLoaded;

	// complete all memory locks
	for(auto it = memory->lockQueue.frontIter(); it.okay(); ++it) {
		Memory::LockRequest *lock_request = &(*it);
		if(!memory->checkLock(lock_request))
			continue;
		
		memory->performLock(lock_request);
		memory->lockQueue.remove(it);
	}

	// resume all waiting threads
	while(!memory->waitQueue.empty()) {
		KernelSharedPtr<Thread> waiting = memory->waitQueue.removeFront();

		ScheduleGuard schedule_guard(scheduleLock.get());
		enqueueInSchedule(schedule_guard, waiting);
		schedule_guard.unlock();
	}
	
	return kHelErrNone;
}

HelError helSubmitLockMemory(HelHandle handle, HelHandle hub_handle,
		uintptr_t offset, size_t size,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto memory_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
		
		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}

	SubmitInfo submit_info(allocAsyncId(), submit_function, submit_object);
	Memory::LockRequest lock_request(offset, size, frigg::move(event_hub), submit_info);
	if(memory->checkLock(&lock_request)) {
		memory->performLock(&lock_request);
	}else{
		memory->loadMemory(offset, size);
		memory->lockQueue.addBack(frigg::move(lock_request));
	}

	return kHelErrNone;
}

HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto memory_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
	}

	if(memory->getType() == Memory::kTypeBacked)
		memory->loadMemory(offset, length);
	
	return kHelErrNone;
}

HelError helCreateThread(HelHandle space_handle, HelHandle directory_handle,
		HelThreadState *user_state, uint32_t flags, HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
	frigg::SharedPtr<RdFolder> directory;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().toShared();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}

		if(directory_handle == kHelNullHandle) {
			directory = this_thread->getDirectory().toShared();
		}else{
			auto dir_wrapper = this_universe->getDescriptor(universe_guard, directory_handle);
			if(!dir_wrapper)
				return kHelErrNoDescriptor;
			if(!dir_wrapper->is<RdDescriptor>())
				return kHelErrBadDescriptor;
			auto &dir_desc = dir_wrapper->get<RdDescriptor>();
			directory = dir_desc.getFolder().toShared();
		}
	}

	KernelSharedPtr<Universe> universe;
	if(flags & kHelThreadNewUniverse) {
		universe = frigg::makeShared<Universe>(*kernelAlloc);
	}else{
		universe = this_universe.toShared();
	}

	KernelSharedPtr<ThreadGroup> group;
	if(flags & kHelThreadNewGroup) {
		group = frigg::makeShared<ThreadGroup>(*kernelAlloc);
	}else{
		group = this_thread->getThreadGroup().toShared();
	}

	auto new_thread = frigg::makeShared<Thread>(*kernelAlloc, frigg::move(universe),
			frigg::move(space), frigg::move(directory));
	if((flags & kHelThreadExclusive) != 0)
		new_thread->flags |= Thread::kFlagExclusive;
	
	ThreadGroup::addThreadToGroup(frigg::move(group),
			KernelWeakPtr<Thread>(new_thread));
	
	// FIXME: do not heap-allocate the state structs
	void *state = kernelAlloc->allocate(getStateSize());
	auto gpr_state = accessGprState(state);
	gpr_state->rax = user_state->rax;
	gpr_state->rbx = user_state->rbx;
	gpr_state->rcx = user_state->rcx;
	gpr_state->rdx = user_state->rdx;
	gpr_state->rsi = user_state->rsi;
	gpr_state->rdi = user_state->rdi;
	gpr_state->rbp = user_state->rbp;

	gpr_state->r8 = user_state->r8;
	gpr_state->r9 = user_state->r9;
	gpr_state->r10 = user_state->r10;
	gpr_state->r11 = user_state->r11;
	gpr_state->r12 = user_state->r12;
	gpr_state->r13 = user_state->r13;
	gpr_state->r14 = user_state->r14;
	gpr_state->r15 = user_state->r15;

	gpr_state->rip = user_state->rip;
	gpr_state->rsp = user_state->rsp;
	gpr_state->rflags = 0x200; // set the interrupt flag
	gpr_state->kernel = 0;
	new_thread->accessSaveState().restoreState = state;
	
	KernelUnsafePtr<Thread> new_thread_ptr(new_thread);
	activeList->addBack(KernelSharedPtr<Thread>(new_thread));

	ScheduleGuard schedule_guard(scheduleLock.get());
	enqueueInSchedule(schedule_guard, new_thread_ptr);
	schedule_guard.unlock();

	{
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				ThreadDescriptor(frigg::move(new_thread)));
	}

	return kHelErrNone;
}

HelError helYield() {
	assert(!intsAreEnabled());

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();

	void *state = __builtin_alloca(getStateSize());
	if(forkState(state)) {
		resetCurrentThread(state);
	
		ScheduleGuard schedule_guard(scheduleLock.get());
		if(!(this_thread->flags & Thread::kFlagNotScheduled))
			enqueueInSchedule(schedule_guard, this_thread);
		doSchedule(frigg::move(schedule_guard));
	}

	return kHelErrNone;
}

HelError helSubmitJoin(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Thread> thread;
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);
		auto thread_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
		
		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}

	SubmitInfo submit_info(allocAsyncId(), submit_function, submit_object);

	thread->submitJoin(frigg::move(event_hub), submit_info);

	*async_id = submit_info.asyncId;
	return kHelErrNone;
}

HelError helExitThisThread() {
	callOnCpuStack(&dropCurrentThread);
}

HelError helWriteFsBase(void *pointer) {
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, (uintptr_t)pointer);
	return kHelErrNone;
}

HelError helGetClock(uint64_t *counter) {
	*counter = currentNanos();
	return kHelErrNone;
}

HelError helCreateSignal(void *entry, HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto signal = frigg::makeShared<Signal>(*kernelAlloc, entry);

	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			SignalDescriptor(frigg::move(signal)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helRaiseSignal(HelHandle handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	Universe::Guard universe_guard(&universe->lock);
	auto signal_wrapper = universe->getDescriptor(universe_guard, handle);
	if(!signal_wrapper)
		return kHelErrNoDescriptor;
	if(!signal_wrapper->is<SignalDescriptor>())
		return kHelErrBadDescriptor;
	auto &signal_descriptor = signal_wrapper->get<SignalDescriptor>();
	KernelSharedPtr<Signal> signal = signal_descriptor.signal;
	universe_guard.unlock();

	this_thread->queueSignal(signal->entry);

	return kHelErrNone;
}

HelError helReturnFromSignal() {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	
	auto *syscall_state = this_thread->accessSaveState().accessSyscallBaseState();

	// TODO: lock user memory

	AdditionalSyscallState additional;
	auto stack = (uint64_t *)syscall_state->rsp;
	additional.rbp = *(stack++);
	additional.r15 = *(stack++);
	additional.rdx = *(stack++);
	additional.rsi = *(stack++);
	additional.rdi = *(stack++);
	syscall_state->rflags = *(stack++);
	syscall_state->rip = *(stack++);
	syscall_state->rsp = *(stack++);

	jumpFromSyscall(&additional);
}

HelError helCreateEventHub(HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto event_hub = frigg::makeShared<EventHub>(*kernelAlloc);

	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			EventHubDescriptor(frigg::move(event_hub)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helWaitForEvents(HelHandle handle,
		HelEvent *user_list, size_t max_items,
		HelNanotime max_nanotime, size_t *num_items) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto hub_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}

	// TODO: check userspace page access rights

	EventHub::Guard hub_guard(&event_hub->lock);
	if(max_nanotime == kHelWaitInfinite) {
		while(!event_hub->hasEvent(hub_guard))
			event_hub->blockCurrentThread(hub_guard);
	}else if(max_nanotime > 0) {
		uint64_t deadline = currentTicks() + durationToTicks(0, 0, 0, max_nanotime);

		Timer timer(deadline);
		timer.thread = this_thread.toWeak();
		installTimer(frigg::move(timer));

		while(!event_hub->hasEvent(hub_guard) && currentTicks() < deadline)
			event_hub->blockCurrentThread(hub_guard);
	}else if(max_nanotime < 0) {
		assert(!"Illegal time parameter");
	}

	size_t count; 
	for(count = 0; count < max_items; count++) {
		if(!event_hub->hasEvent(hub_guard))
			break;
		frigg::SharedPtr<AsyncOperation> operation = event_hub->dequeueEvent(hub_guard);
		UserEvent event = operation->getEvent();

		HelEvent *user_evt = &user_list[count];
		switch(event.type) {
		case UserEvent::kTypeMemoryLoad: {
			user_evt->type = kHelEventLoadMemory;
			user_evt->error = kHelErrNone;
			user_evt->offset = event.offset;
			user_evt->length = event.length;
		} break;
		case UserEvent::kTypeMemoryLock: {
			user_evt->type = kHelEventLockMemory;
			user_evt->error = kHelErrNone;
		} break;
		case UserEvent::kTypeJoin: {
			user_evt->type = kHelEventJoin;
			user_evt->error = kHelErrNone;
		} break;
		case UserEvent::kTypeError: {
			user_evt->type = kHelEventRecvString;

			switch(event.error) {
			case kErrPipeClosed:
				user_evt->error = kHelErrPipeClosed;
				break;
			case kErrBufferTooSmall:
				user_evt->error = kHelErrBufferTooSmall;
				break;
			default:
				assert(!"Unexpected error");
			}
		} break;
		case UserEvent::kTypeSendString: {
			user_evt->type = kHelEventSendString;
			user_evt->error = kHelErrNone;
		} break;
		case UserEvent::kTypeSendDescriptor: {
			user_evt->type = kHelEventSendDescriptor;
			user_evt->error = kHelErrNone;
		} break;
		case UserEvent::kTypeRecvString: {
			user_evt->type = kHelEventRecvString;
			user_evt->error = kHelErrNone;
			user_evt->msgRequest = event.msgRequest;
			user_evt->msgSequence = event.msgSequence;
			user_evt->length = event.length;
		} break;
		case UserEvent::kTypeRecvStringToRing: {
			user_evt->type = kHelEventRecvStringToQueue;
			user_evt->error = kHelErrNone;
			user_evt->msgRequest = event.msgRequest;
			user_evt->msgSequence = event.msgSequence;
			user_evt->length = event.length;
			user_evt->offset = event.offset;
		} break;
		case UserEvent::kTypeRecvDescriptor: {
			user_evt->type = kHelEventRecvDescriptor;
			user_evt->error = kHelErrNone;
			user_evt->msgRequest = event.msgRequest;
			user_evt->msgSequence = event.msgSequence;
			user_evt->handle = event.handle;
		} break;
		case UserEvent::kTypeAccept: {
			user_evt->type = kHelEventAccept;
			user_evt->error = kHelErrNone;
			user_evt->handle = event.handle;
		} break;
		case UserEvent::kTypeConnect: {
			user_evt->type = kHelEventConnect;
			user_evt->error = kHelErrNone;
			user_evt->handle = event.handle;
		} break;
		case UserEvent::kTypeIrq: {
			user_evt->type = kHelEventIrq;
			user_evt->error = kHelErrNone;
		} break;
		default:
			assert(!"Illegal event type");
		}

		user_evt->asyncId = event.submitInfo.asyncId;
		user_evt->submitFunction = event.submitInfo.submitFunction;
		user_evt->submitObject = event.submitInfo.submitObject;
	}
	hub_guard.unlock();

	*num_items = count;

	return kHelErrNone;
}


HelError helCreateRing(size_t max_chunk_size, HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto ring = frigg::makeShared<RingBuffer>(*kernelAlloc);

	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			RingDescriptor(frigg::move(ring)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helSubmitRing(HelHandle handle, HelHandle hub_handle,
		struct HelRingBuffer *buffer, size_t buffer_size,
		uintptr_t submit_function, uintptr_t submit_object,
		int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<RingBuffer> ring;
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto ring_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!ring_wrapper)
			return kHelErrNoDescriptor;
		if(!ring_wrapper->is<RingDescriptor>())
			return kHelErrBadDescriptor;
		ring = ring_wrapper->get<RingDescriptor>().ringBuffer;

		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}
	
	frigg::SharedPtr<AddressSpace> space = this_thread->getAddressSpace().toShared();
	auto space_lock = DirectSpaceLock<HelRingBuffer>::acquire(frigg::move(space), buffer);

	AsyncData data(event_hub, allocAsyncId(), submit_function, submit_object);
	*async_id = data.asyncId;

	auto ring_item = frigg::makeShared<AsyncRingItem>(*kernelAlloc,
			frigg::move(data), frigg::move(space_lock), buffer_size);
	ring->submitBuffer(frigg::move(ring_item));
	
	return kHelErrNone;
}


HelError helCreateFullPipe(HelHandle *first_handle,
		HelHandle *second_handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	KernelSharedPtr<FullPipe> pipe;
	KernelSharedPtr<Endpoint> end1, end2;
	FullPipe::create(pipe, end1, end2);

	Universe::Guard universe_guard(&universe->lock);
	*first_handle = universe->attachDescriptor(universe_guard,
			EndpointDescriptor(frigg::move(end1)));
	*second_handle = universe->attachDescriptor(universe_guard,
			EndpointDescriptor(frigg::move(end2)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helSubmitSendString(HelHandle handle, HelHandle hub_handle,
		const void *user_buffer, size_t length,
		int64_t msg_request, int64_t msg_sequence,
		uintptr_t submit_function, uintptr_t submit_object,
		uint32_t flags, int64_t *async_id) {
	if(flags & ~(kHelRequest | kHelResponse))
		return kHelErrIllegalArgs;
	
	if(!(flags & kHelRequest) && !(flags & kHelResponse))
		return kHelErrIllegalArgs;

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights
	
	frigg::SharedPtr<Endpoint> endpoint;
	frigg::SharedPtr<EventHub> event_hub;
	AnyDescriptor send_descriptor;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto end_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!end_wrapper)
			return kHelErrNoDescriptor;
		if(!end_wrapper->is<EndpointDescriptor>())
			return kHelErrBadDescriptor;
		endpoint = end_wrapper->get<EndpointDescriptor>().endpoint;
		
		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}
	
	size_t write_index = endpoint->getWriteIndex();
	Channel &channel = endpoint->getPipe()->getChannel(write_index);

	uint32_t send_flags = 0;
	if(flags & kHelRequest)
		send_flags |= Channel::kFlagRequest;
	if(flags & kHelResponse)
		send_flags |= Channel::kFlagResponse;

	frigg::UniqueMemory<KernelAlloc> kernel_buffer(*kernelAlloc, length);
	memcpy(kernel_buffer.data(), user_buffer, length);
	
	AsyncData data(event_hub, allocAsyncId(), submit_function, submit_object);
	*async_id = data.asyncId;
	
	auto send = frigg::makeShared<AsyncSendString>(*kernelAlloc,
			frigg::move(data), msg_request, msg_sequence);
	send->flags = send_flags;
	send->kernelBuffer = frigg::move(kernel_buffer);
	
	Error error;
	{
		Channel::Guard channel_guard(&channel.lock);
		error = channel.sendString(channel_guard, frigg::move(send));
	}

	if(error == kErrPipeClosed)
		return kHelErrPipeClosed;

	assert(error == kErrSuccess);
	return kHelErrNone;
}

HelError helSubmitSendDescriptor(HelHandle handle, HelHandle hub_handle,
		HelHandle send_handle, int64_t msg_request, int64_t msg_sequence,
		uintptr_t submit_function, uintptr_t submit_object,
		uint32_t flags, int64_t *async_id) {
	if(flags & ~(kHelRequest | kHelResponse))
		return kHelErrIllegalArgs;
	
	if(!(flags & kHelRequest) && !(flags & kHelResponse))
		return kHelErrIllegalArgs;

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights
	
	frigg::SharedPtr<Endpoint> endpoint;
	frigg::SharedPtr<EventHub> event_hub;
	AnyDescriptor send_descriptor;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto end_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!end_wrapper)
			return kHelErrNoDescriptor;
		if(!end_wrapper->is<EndpointDescriptor>())
			return kHelErrBadDescriptor;
		endpoint = end_wrapper->get<EndpointDescriptor>().endpoint;
		
		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
		
		auto send_wrapper = universe->getDescriptor(universe_guard, send_handle);
		if(!send_wrapper)
			return kHelErrNoDescriptor;
		send_descriptor = AnyDescriptor(*send_wrapper);
	}
	
	size_t write_index = endpoint->getWriteIndex();
	Channel &channel = endpoint->getPipe()->getChannel(write_index);

	uint32_t send_flags = 0;
	if(flags & kHelRequest)
		send_flags |= Channel::kFlagRequest;
	if(flags & kHelResponse)
		send_flags |= Channel::kFlagResponse;
	
	AsyncData data(event_hub, allocAsyncId(), submit_function, submit_object);
	*async_id = data.asyncId;
	
	auto send = frigg::makeShared<AsyncSendDescriptor>(*kernelAlloc,
			frigg::move(data), msg_request, msg_sequence);
	send->flags = send_flags;
	send->descriptor = frigg::move(send_descriptor);

	Error error;
	{
		Channel::Guard channel_guard(&channel.lock);
		error = channel.sendDescriptor(channel_guard, frigg::move(send));
	}

	if(error == kErrPipeClosed)
		return kHelErrPipeClosed;

	assert(error == kErrSuccess);
	return kHelErrNone;
}

HelError helSubmitRecvString(HelHandle handle,
		HelHandle hub_handle, void *user_buffer, size_t max_length,
		int64_t filter_request, int64_t filter_sequence,
		uintptr_t submit_function, uintptr_t submit_object,
		uint32_t flags, int64_t *async_id) {
	if(flags & ~(kHelRequest | kHelResponse))
		return kHelErrIllegalArgs;
	
	if(!(flags & kHelRequest) && !(flags & kHelResponse))
		return kHelErrIllegalArgs;

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Endpoint> endpoint;
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto endpoint_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!endpoint_wrapper)
			return kHelErrNoDescriptor;
		if(!endpoint_wrapper->is<EndpointDescriptor>())
			return kHelErrBadDescriptor;
		endpoint = endpoint_wrapper->get<EndpointDescriptor>().endpoint;

		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}

	size_t read_index = endpoint->getReadIndex();
	Channel &channel = endpoint->getPipe()->getChannel(read_index);

	uint32_t recv_flags = 0;
	if(flags & kHelRequest)
		recv_flags |= Channel::kFlagRequest;
	if(flags & kHelResponse)
		recv_flags |= Channel::kFlagResponse;

	frigg::SharedPtr<AddressSpace> space = this_thread->getAddressSpace().toShared();
	auto space_lock = ForeignSpaceLock::acquire(frigg::move(space), user_buffer, max_length);

	AsyncData data(event_hub, allocAsyncId(), submit_function, submit_object);
	*async_id = data.asyncId;

	auto recv = frigg::makeShared<AsyncRecvString>(*kernelAlloc, frigg::move(data),
			AsyncRecvString::kTypeNormal, filter_request, filter_sequence);
	recv->flags = recv_flags;
	recv->spaceLock = frigg::move(space_lock);

	Channel::Guard channel_guard(&channel.lock);
	Error error = channel.submitRecvString(channel_guard, frigg::move(recv));
	channel_guard.unlock();

	if(error == kErrPipeClosed)
		return kHelErrPipeClosed;

	assert(error == kErrSuccess);
	return kHelErrNone;
}

HelError helSubmitRecvStringToRing(HelHandle handle,
		HelHandle hub_handle, HelHandle ring_handle,
		int64_t filter_request, int64_t filter_sequence,
		uintptr_t submit_function, uintptr_t submit_object,
		uint32_t flags, int64_t *async_id) {
	if(flags & ~(kHelRequest | kHelResponse))
		return kHelErrIllegalArgs;
	
	if(!(flags & kHelRequest) && !(flags & kHelResponse))
		return kHelErrIllegalArgs;

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Endpoint> endpoint;
	frigg::SharedPtr<EventHub> event_hub;
	frigg::SharedPtr<RingBuffer> ring;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;

		auto ring_wrapper = universe->getDescriptor(universe_guard, ring_handle);
		if(!ring_wrapper)
			return kHelErrNoDescriptor;
		if(!ring_wrapper->is<RingDescriptor>())
			return kHelErrBadDescriptor;
		ring = ring_wrapper->get<RingDescriptor>().ringBuffer;

		auto endpoint_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!endpoint_wrapper)
			return kHelErrNoDescriptor;
		if(!endpoint_wrapper->is<EndpointDescriptor>())
			return kHelErrBadDescriptor;
		endpoint = endpoint_wrapper->get<EndpointDescriptor>().endpoint;
	}

	size_t read_index = endpoint->getReadIndex();
	Channel &channel = endpoint->getPipe()->getChannel(read_index);

	uint32_t recv_flags = 0;
	if(flags & kHelRequest)
		recv_flags |= Channel::kFlagRequest;
	if(flags & kHelResponse)
		recv_flags |= Channel::kFlagResponse;

	AsyncData data(event_hub, allocAsyncId(), submit_function, submit_object);
	*async_id = data.asyncId;

	auto recv = frigg::makeShared<AsyncRecvString>(*kernelAlloc, frigg::move(data),
			AsyncRecvString::kTypeToRing, filter_request, filter_sequence);
	recv->flags = recv_flags;
	recv->ringBuffer = frigg::move(ring);
	
	Channel::Guard channel_guard(&channel.lock);
	Error error = channel.submitRecvString(channel_guard, frigg::move(recv));
	channel_guard.unlock();

	if(error == kErrPipeClosed)
		return kHelErrPipeClosed;

	assert(error == kErrSuccess);
	return kHelErrNone;
}

HelError helSubmitRecvDescriptor(HelHandle handle,
		HelHandle hub_handle,
		int64_t filter_request, int64_t filter_sequence,
		uintptr_t submit_function, uintptr_t submit_object,
		uint32_t flags, int64_t *async_id) {
	if(flags & ~(kHelRequest | kHelResponse))
		return kHelErrIllegalArgs;
	
	if(!(flags & kHelRequest) && !(flags & kHelResponse))
		return kHelErrIllegalArgs;

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Endpoint> endpoint;
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto wrapper = universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<EndpointDescriptor>())
			return kHelErrBadDescriptor;
		endpoint = wrapper->get<EndpointDescriptor>().endpoint;

		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}

	size_t read_index = endpoint->getReadIndex();
	Channel &channel = endpoint->getPipe()->getChannel(read_index);

	uint32_t recv_flags = 0;
	if(flags & kHelRequest)
		recv_flags |= Channel::kFlagRequest;
	if(flags & kHelResponse)
		recv_flags |= Channel::kFlagResponse;

	AsyncData data(event_hub, allocAsyncId(), submit_function, submit_object);
	*async_id = data.asyncId;

	auto recv = frigg::makeShared<AsyncRecvDescriptor>(*kernelAlloc, frigg::move(data),
			universe.toWeak(), filter_request, filter_sequence);
	recv->flags = recv_flags;
	
	Channel::Guard channel_guard(&channel.lock);
	Error error = channel.submitRecvDescriptor(channel_guard, frigg::move(recv));
	channel_guard.unlock();
	
	if(error == kErrPipeClosed)
		return kHelErrPipeClosed;
	
	assert(error == kErrSuccess);
	return kHelErrNone;
}


HelError helCreateServer(HelHandle *server_handle, HelHandle *client_handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto server = frigg::makeShared<Server>(*kernelAlloc);
	KernelSharedPtr<Server> copy(server);

	Universe::Guard universe_guard(&universe->lock);
	*server_handle = universe->attachDescriptor(universe_guard,
			ServerDescriptor(frigg::move(server)));
	*client_handle = universe->attachDescriptor(universe_guard,
			ClientDescriptor(frigg::move(copy)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helSubmitAccept(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Server> server;
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto server_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!server_wrapper)
			return kHelErrNoDescriptor;
		if(!server_wrapper->is<ServerDescriptor>())
			return kHelErrBadDescriptor;
		server = server_wrapper->get<ServerDescriptor>().server;

		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}
	
	AsyncData data(event_hub, allocAsyncId(), submit_function, submit_object);
	*async_id = data.asyncId;
	
	auto request = frigg::makeShared<AsyncAccept>(*kernelAlloc,
			frigg::move(data), universe.toWeak());
	{
		Server::Guard server_guard(&server->lock);
		server->submitAccept(server_guard, frigg::move(request));
	}

	return kHelErrNone;
}

HelError helSubmitConnect(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Server> server;
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto connect_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!connect_wrapper)
			return kHelErrNoDescriptor;
		if(!connect_wrapper->is<ClientDescriptor>())
			return kHelErrBadDescriptor;
		server = connect_wrapper->get<ClientDescriptor>().server;

		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}

	AsyncData data(event_hub, allocAsyncId(), submit_function, submit_object);
	*async_id = data.asyncId;
	
	auto request = frigg::makeShared<AsyncConnect>(*kernelAlloc,
			frigg::move(data), universe.toWeak());
	{
		Server::Guard server_guard(&server->lock);
		server->submitConnect(server_guard, frigg::move(request));
	}

	return kHelErrNone;
}


HelError helCreateRd(HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto folder = frigg::makeShared<RdFolder>(*kernelAlloc);

	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			RdDescriptor(frigg::move(folder)));
	universe_guard.unlock();
	
	return kHelErrNone;
}

HelError helRdMount(HelHandle handle, const char *user_name,
		size_t name_length, HelHandle mount_handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	Universe::Guard universe_guard(&universe->lock);
	auto dir_wrapper = universe->getDescriptor(universe_guard, handle);
	if(!dir_wrapper)
		return kHelErrNoDescriptor;
	if(!dir_wrapper->is<RdDescriptor>())
		return kHelErrBadDescriptor;
	auto &dir_desc = dir_wrapper->get<RdDescriptor>();
	KernelSharedPtr<RdFolder> directory = dir_desc.getFolder().toShared();

	auto mount_wrapper = universe->getDescriptor(universe_guard, mount_handle);
	if(!mount_wrapper)
		return kHelErrNoDescriptor;
	if(!mount_wrapper->is<RdDescriptor>())
		return kHelErrBadDescriptor;
	auto &mount_desc = mount_wrapper->get<RdDescriptor>();
	KernelSharedPtr<RdFolder> mount_directory = mount_desc.getFolder().toShared();
	universe_guard.unlock();

	directory->mount(user_name, name_length, frigg::move(mount_directory));
	
	return kHelErrNone;
}

HelError helRdPublish(HelHandle handle, const char *user_name,
		size_t name_length, HelHandle publish_handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	Universe::Guard universe_guard(&universe->lock);
	auto dir_wrapper = universe->getDescriptor(universe_guard, handle);
	if(!dir_wrapper)
		return kHelErrNoDescriptor;
	if(!dir_wrapper->is<RdDescriptor>())
		return kHelErrBadDescriptor;
	auto &dir_desc = dir_wrapper->get<RdDescriptor>();
	KernelSharedPtr<RdFolder> directory = dir_desc.getFolder().toShared();
	
	// copy the descriptor we want to publish
	auto publish_wrapper = universe->getDescriptor(universe_guard, publish_handle);
	if(!publish_wrapper)
		return kHelErrNoDescriptor;
	AnyDescriptor publish_copy(*publish_wrapper);
	universe_guard.unlock();

	directory->publish(user_name, name_length, frigg::move(publish_copy));
	
	return kHelErrNone;
}

HelError helRdOpen(const char *user_name, size_t name_length, HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	// TODO: verifiy access rights for user_name
	
	auto find_char = [] (const char *string, char c,
			size_t start_at, size_t max_length) -> size_t {
		for(size_t i = start_at; i < max_length; i++)
			if(string[i] == c)
				return i;
		return max_length;
	};
	
	KernelUnsafePtr<RdFolder> directory = this_thread->getDirectory();
	
	size_t search_from = 0;
	while(true) {
		size_t next_slash = find_char(user_name, '/', search_from, name_length);
		frigg::StringView part(user_name + search_from, next_slash - search_from);
		if(next_slash == name_length) {
			if(part == frigg::StringView("#this")) {
				// open a handle to this directory
				Universe::Guard universe_guard(&universe->lock);
				*handle = universe->attachDescriptor(universe_guard,
						RdDescriptor(directory.toShared()));
				universe_guard.unlock();

				return kHelErrNone;
			}else{
				// read a file from this directory
				frigg::Optional<RdFolder::Entry *> entry = directory->getEntry(part.data(), part.size());
				if(!entry)
					return kHelErrNoSuchPath;

				AnyDescriptor copy((*entry)->descriptor);
				
				Universe::Guard universe_guard(&universe->lock);
				*handle = universe->attachDescriptor(universe_guard, frigg::move(copy));
				universe_guard.unlock();

				return kHelErrNone;
			}
		}else{
			// read a subdirectory of this directory
			frigg::Optional<RdFolder::Entry *> entry = directory->getEntry(part.data(), part.size());
			if(!entry)
				return kHelErrNoSuchPath;

			directory = (*entry)->mounted;
		}
		search_from = next_slash + 1;
	}
}


HelError helAccessIrq(int number, HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto irq_line = frigg::makeShared<IrqLine>(*kernelAlloc, number);
	
	IrqRelay::Guard irq_guard(&irqRelays[number]->lock);
	irqRelays[number]->addLine(irq_guard, frigg::WeakPtr<IrqLine>(irq_line));
	irq_guard.unlock();

	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			IrqDescriptor(frigg::move(irq_line)));
	universe_guard.unlock();

	return kHelErrNone;
}
HelError helSetupIrq(HelHandle handle, uint32_t flags) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<IrqLine> irq_line;
	{
		Universe::Guard universe_guard(&universe->lock);
		auto irq_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq_line = irq_wrapper->get<IrqDescriptor>().irqLine;
	}

	uint32_t relay_flags = 0;
	if(flags & kHelIrqManualAcknowledge)
		relay_flags |= IrqRelay::kFlagManualAcknowledge;
	
	int number = irq_line->getNumber();

	IrqRelay::Guard relay_guard(&irqRelays[number]->lock);
	irqRelays[number]->setup(relay_guard, relay_flags);
	relay_guard.unlock();

	return kHelErrNone;
}
HelError helAcknowledgeIrq(HelHandle handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<IrqLine> irq_line;
	{
		Universe::Guard universe_guard(&universe->lock);
		auto irq_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq_line = irq_wrapper->get<IrqDescriptor>().irqLine;
	}

	int number = irq_line->getNumber();
	
	IrqRelay::Guard relay_guard(&irqRelays[number]->lock);
	irqRelays[number]->manualAcknowledge(relay_guard);
	relay_guard.unlock();

	return kHelErrNone;
}
HelError helSubmitWaitForIrq(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	frigg::SharedPtr<IrqLine> line;
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto irq_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		line = irq_wrapper->get<IrqDescriptor>().irqLine;
		
		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}

	SubmitInfo submit_info(allocAsyncId(), submit_function, submit_object);

	IrqLine::Guard line_guard(&line->lock);
	line->submitWait(line_guard, frigg::move(event_hub), submit_info);
	line_guard.unlock();

	*async_id = submit_info.asyncId;
	return kHelErrNone;
}
HelError helSubscribeIrq(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<IrqLine> line;
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);
		auto irq_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		line = irq_wrapper->get<IrqDescriptor>().irqLine;
		
		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}

	SubmitInfo submit_info(allocAsyncId(), submit_function, submit_object);

	IrqLine::Guard line_guard(&line->lock);
	line->subscribe(line_guard, frigg::move(event_hub), submit_info);
	line_guard.unlock();

	*async_id = submit_info.asyncId;
	return kHelErrNone;
}

HelError helAccessIo(uintptr_t *user_port_array, size_t num_ports,
		HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights
	auto io_space = frigg::makeShared<IoSpace>(*kernelAlloc);
	for(size_t i = 0; i < num_ports; i++)
		io_space->addPort(user_port_array[i]);

	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			IoDescriptor(frigg::move(io_space)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helEnableIo(HelHandle handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<IoSpace> io_space;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto wrapper = universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<IoDescriptor>())
			return kHelErrBadDescriptor;
		io_space = wrapper->get<IoDescriptor>().ioSpace;
	}

	io_space->enableInThread(this_thread);

	return kHelErrNone;
}

HelError helEnableFullIo() {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();

	for(uintptr_t port = 0; port < 0x10000; port++)
		this_thread->enableIoPort(port);

	return kHelErrNone;
}

