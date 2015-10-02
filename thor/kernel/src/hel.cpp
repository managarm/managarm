
#include "kernel.hpp"
#include "../../hel/include/hel.h"

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
	switch((*wrapper)->tag()) {
	case AnyDescriptor::tagOf<BiDirectionFirstDescriptor>():
	case AnyDescriptor::tagOf<BiDirectionSecondDescriptor>():
		user_info->type = kHelDescChannel;
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
	assert((size % kPageSize) == 0);

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	auto memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeAllocated);
	memory->resize(size / kPageSize);
	
	if(flags & kHelAllocOnDemand) {
		memory->flags |= Memory::kFlagOnDemand;
	}else{
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		for(size_t i = 0; i < memory->numPages(); i++)
			memory->setPage(i, physicalAllocator->allocate(physical_guard, 1));
		physical_guard.unlock();
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
		memory->setPage(i, physical + i * kPageSize);
	
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
	
	Universe::Guard universe_guard(&universe->lock);
	KernelSharedPtr<AddressSpace> space;
	if(handle == kHelNullHandle) {
		space = KernelSharedPtr<AddressSpace>(this_thread->getAddressSpace());
	}else{
		auto space_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!space_wrapper)
			return kHelErrNoDescriptor;
		if(!(*space_wrapper)->is<AddressSpaceDescriptor>())
			return kHelErrBadDescriptor;
		auto &space_desc = (*space_wrapper)->get<AddressSpaceDescriptor>();
		space = KernelSharedPtr<AddressSpace>(space_desc.getSpace());
	}
	universe_guard.unlock();

	AddressSpace::Guard space_guard(&space->lock);
	auto forked = space->fork(space_guard);
	space_guard.unlock();
	
	universe_guard.lock();
	*forked_handle = universe->attachDescriptor(universe_guard,
			AddressSpaceDescriptor(frigg::move(forked)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helMapMemory(HelHandle memory_handle, HelHandle space_handle,
		void *pointer, size_t length, uint32_t flags, void **actual_pointer) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&universe->lock);
	auto memory_wrapper = universe->getDescriptor(universe_guard, memory_handle);
	if(!memory_wrapper)
		return kHelErrNoDescriptor;
	if(!(*memory_wrapper)->is<MemoryAccessDescriptor>())
		return kHelErrBadDescriptor;
	auto &memory_desc = (*memory_wrapper)->get<MemoryAccessDescriptor>();
	KernelSharedPtr<Memory> memory(memory_desc.getMemory());
	
	KernelSharedPtr<AddressSpace> space;
	if(space_handle == kHelNullHandle) {
		space = KernelSharedPtr<AddressSpace>(this_thread->getAddressSpace());
	}else{
		auto space_wrapper = universe->getDescriptor(universe_guard, space_handle);
		if(!space_wrapper)
			return kHelErrNoDescriptor;
		if(!(*space_wrapper)->is<AddressSpaceDescriptor>())
			return kHelErrBadDescriptor;
		auto &space_desc = (*space_wrapper)->get<AddressSpaceDescriptor>();
		space = KernelSharedPtr<AddressSpace>(space_desc.getSpace());
	}
	universe_guard.unlock();

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
	
	VirtualAddr actual_address;
	AddressSpace::Guard space_guard(&space->lock);
	space->map(space_guard, memory, (VirtualAddr)pointer, length,
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
		space = KernelSharedPtr<AddressSpace>(this_thread->getAddressSpace());
	}else{
		auto space_wrapper = universe->getDescriptor(universe_guard, space_handle);
		if(!space_wrapper)
			return kHelErrNoDescriptor;
		if(!(*space_wrapper)->is<AddressSpaceDescriptor>())
			return kHelErrBadDescriptor;
		auto &space_desc = (*space_wrapper)->get<AddressSpaceDescriptor>();
		space = KernelSharedPtr<AddressSpace>(space_desc.getSpace());
	}
	universe_guard.unlock();
	
	AddressSpace::Guard space_guard(&space->lock);
	space->unmap(space_guard, (VirtualAddr)pointer, length);
	space_guard.unlock();

	return kHelErrNone;
}

HelError helMemoryInfo(HelHandle handle, size_t *size) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&universe->lock);
	auto wrapper = universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	if(!(*wrapper)->is<MemoryAccessDescriptor>())
		return kHelErrBadDescriptor;
	auto &descriptor = (*wrapper)->get<MemoryAccessDescriptor>();
	KernelUnsafePtr<Memory> memory = descriptor.getMemory();

	*size = memory->numPages() * kPageSize;
	universe_guard.unlock();

	return kHelErrNone;
}


HelError helCreateThread(HelHandle space_handle, HelHandle directory_handle,
		HelThreadState *user_state, uint32_t flags, HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> this_universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&this_universe->lock);
	KernelSharedPtr<AddressSpace> address_space;
	if(space_handle == kHelNullHandle) {
		address_space = KernelSharedPtr<AddressSpace>(this_thread->getAddressSpace());
	}else{
		auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
		if(!space_wrapper)
			return kHelErrNoDescriptor;
		if(!(*space_wrapper)->is<AddressSpaceDescriptor>())
			return kHelErrBadDescriptor;
		auto &space_desc = (*space_wrapper)->get<AddressSpaceDescriptor>();
		address_space = KernelSharedPtr<AddressSpace>(space_desc.getSpace());
	}

	KernelSharedPtr<RdFolder> directory;
	if(directory_handle == kHelNullHandle) {
		directory = KernelSharedPtr<RdFolder>(this_thread->getDirectory());
	}else{
		auto dir_wrapper = this_universe->getDescriptor(universe_guard, directory_handle);
		if(!dir_wrapper)
			return kHelErrNoDescriptor;
		if(!(*dir_wrapper)->is<RdDescriptor>())
			return kHelErrBadDescriptor;
		auto &dir_desc = (*dir_wrapper)->get<RdDescriptor>();
		directory = KernelSharedPtr<RdFolder>(dir_desc.getFolder());
	}
	universe_guard.unlock();
	
	KernelSharedPtr<Universe> universe;
	if((flags & kHelThreadNewUniverse) != 0) {
		universe = frigg::makeShared<Universe>(*kernelAlloc);
	}else{
		universe = KernelSharedPtr<Universe>(this_universe);
	}

	auto new_thread = frigg::makeShared<Thread>(*kernelAlloc, frigg::move(universe),
			frigg::move(address_space), frigg::move(directory));
	if((flags & kHelThreadExclusive) != 0)
		new_thread->flags |= Thread::kFlagExclusive;
	
	auto base_state = new_thread->accessSaveState().accessGeneralBaseState();
	base_state->rax = user_state->rax;
	base_state->rbx = user_state->rbx;
	base_state->rcx = user_state->rcx;
	base_state->rdx = user_state->rdx;
	base_state->rsi = user_state->rsi;
	base_state->rdi = user_state->rdi;
	base_state->rbp = user_state->rbp;

	base_state->r8 = user_state->r8;
	base_state->r9 = user_state->r9;
	base_state->r10 = user_state->r10;
	base_state->r11 = user_state->r11;
	base_state->r12 = user_state->r12;
	base_state->r13 = user_state->r13;
	base_state->r14 = user_state->r14;
	base_state->r15 = user_state->r15;

	base_state->rip = user_state->rip;
	base_state->rsp = user_state->rsp;
	base_state->rflags = 0x200; // set the interrupt flag
	base_state->kernel = 0;
	
	KernelUnsafePtr<Thread> new_thread_ptr(new_thread);
	activeList->addBack(frigg::move(new_thread));

	ScheduleGuard schedule_guard(scheduleLock.get());
	enqueueInSchedule(schedule_guard, new_thread_ptr);
	schedule_guard.unlock();

//	ThreadObserveDescriptor base(frigg::move(new_thread));
//	*handle = universe->attachDescriptor(frigg::move(base));

	return kHelErrNone;
}

HelError helExitThisThread() {
	callOnCpuStack(&dropCurrentThread);
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
	
	Universe::Guard universe_guard(&universe->lock);
	auto hub_wrapper = universe->getDescriptor(universe_guard, handle);
	if(!hub_wrapper)
		return kHelErrNoDescriptor;
	if(!(*hub_wrapper)->is<EventHubDescriptor>())
		return kHelErrBadDescriptor;
	auto &hub_descriptor = (*hub_wrapper)->get<EventHubDescriptor>();
	KernelSharedPtr<EventHub> event_hub(hub_descriptor.getEventHub());
	universe_guard.unlock();

	// TODO: check userspace page access rights

	EventHub::Guard hub_guard(&event_hub->lock);
	if(max_nanotime == kHelWaitInfinite) {
		while(!event_hub->hasEvent(hub_guard))
			event_hub->blockCurrentThread(hub_guard);
	}else if(max_nanotime > 0) {
		uint64_t deadline = currentTicks() + durationToTicks(0, 0, 0, max_nanotime);

		Timer timer(deadline);
		timer.thread = KernelWeakPtr<Thread>(this_thread);
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
		UserEvent event = event_hub->dequeueEvent(hub_guard);

		HelEvent *user_evt = &user_list[count];
		switch(event.type) {
		case UserEvent::kTypeRecvStringTransfer: {
			user_evt->type = kHelEventRecvString;
			user_evt->error = kHelErrNone;
			user_evt->msgRequest = event.msgRequest;
			user_evt->msgSequence = event.msgSequence;

			// TODO: check userspace page access rights
	
			// do the actual memory transfer
			hub_guard.unlock();
			memcpy(event.userBuffer, event.kernelBuffer, event.length);
			hub_guard.lock();
			user_evt->length = event.length;
		} break;
		case UserEvent::kTypeRecvStringError: {
			user_evt->type = kHelEventRecvString;

			switch(event.error) {
			case kErrBufferTooSmall:
				user_evt->error = kHelErrBufferTooSmall;
				break;
			default:
				assert(!"Unexpected error");
			}
		} break;
		case UserEvent::kTypeRecvDescriptor: {
			user_evt->type = kHelEventRecvDescriptor;
			user_evt->error = kHelErrNone;
			user_evt->msgRequest = event.msgRequest;
			user_evt->msgSequence = event.msgSequence;
			
			universe_guard.lock();
			user_evt->handle = universe->attachDescriptor(universe_guard,
					AnyDescriptor(frigg::move(event.descriptor)));
			universe_guard.unlock();
		} break;
		case UserEvent::kTypeAccept: {
			user_evt->type = kHelEventAccept;
			user_evt->error = kHelErrNone;

			universe_guard.lock();
			user_evt->handle = universe->attachDescriptor(universe_guard,
					BiDirectionFirstDescriptor(frigg::move(event.pipe)));
			universe_guard.unlock();
		} break;
		case UserEvent::kTypeConnect: {
			user_evt->type = kHelEventConnect;
			user_evt->error = kHelErrNone;

			universe_guard.lock();
			user_evt->handle = universe->attachDescriptor(universe_guard,
					BiDirectionSecondDescriptor(frigg::move(event.pipe)));
			universe_guard.unlock();
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


HelError helCreateBiDirectionPipe(HelHandle *first_handle,
		HelHandle *second_handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto pipe = frigg::makeShared<BiDirectionPipe>(*kernelAlloc);
	KernelSharedPtr<BiDirectionPipe> copy(pipe);

	Universe::Guard universe_guard(&universe->lock);
	*first_handle = universe->attachDescriptor(universe_guard,
			BiDirectionFirstDescriptor(frigg::move(pipe)));
	*second_handle = universe->attachDescriptor(universe_guard,
			BiDirectionSecondDescriptor(frigg::move(copy)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helSendString(HelHandle handle,
		const uint8_t *user_buffer, size_t length,
		int64_t msg_request, int64_t msg_sequence) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights
	
	Universe::Guard universe_guard(&universe->lock);
	auto wrapper = universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;

	switch((*wrapper)->tag()) {
		case AnyDescriptor::tagOf<BiDirectionFirstDescriptor>(): {
			auto &descriptor = (*wrapper)->get<BiDirectionFirstDescriptor>();
			KernelSharedPtr<BiDirectionPipe> pipe(descriptor.getPipe());
			universe_guard.unlock();

			Channel *channel = pipe->getSecondChannel();
			
			Channel::Guard channel_guard(&channel->lock);
			channel->sendString(channel_guard, user_buffer, length,
					msg_request, msg_sequence);
			channel_guard.unlock();
		} break;
		case AnyDescriptor::tagOf<BiDirectionSecondDescriptor>(): {
			auto &descriptor = (*wrapper)->get<BiDirectionSecondDescriptor>();
			KernelSharedPtr<BiDirectionPipe> pipe(descriptor.getPipe());
			Channel *channel = pipe->getFirstChannel();
			
			Channel::Guard channel_guard(&channel->lock);
			channel->sendString(channel_guard, user_buffer, length,
					msg_request, msg_sequence);
			channel_guard.unlock();
		} break;
		default: {
			return kHelErrBadDescriptor;
		}
	}

	return kHelErrNone;
}

HelError helSendDescriptor(HelHandle handle, HelHandle send_handle,
		int64_t msg_request, int64_t msg_sequence) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights

	Universe::Guard universe_guard(&universe->lock);
	auto wrapper = universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	
	auto send_wrapper = universe->getDescriptor(universe_guard, send_handle);
	if(!send_wrapper)
		return kHelErrNoDescriptor;
	universe_guard.unlock();

	switch((*wrapper)->tag()) {
		case AnyDescriptor::tagOf<BiDirectionFirstDescriptor>(): {
			auto &descriptor = (*wrapper)->get<BiDirectionFirstDescriptor>();
			KernelSharedPtr<BiDirectionPipe> pipe(descriptor.getPipe());
			Channel *channel = pipe->getSecondChannel();

			Channel::Guard channel_guard(&channel->lock);
			channel->sendDescriptor(channel_guard, AnyDescriptor(**send_wrapper),
					msg_request, msg_sequence);
			channel_guard.unlock();
		} break;
		case AnyDescriptor::tagOf<BiDirectionSecondDescriptor>(): {
			auto &descriptor = (*wrapper)->get<BiDirectionSecondDescriptor>();
			KernelSharedPtr<BiDirectionPipe> pipe(descriptor.getPipe());
			Channel *channel = pipe->getFirstChannel();

			Channel::Guard channel_guard(&channel->lock);
			channel->sendDescriptor(channel_guard, AnyDescriptor(**send_wrapper),
					msg_request, msg_sequence);
			channel_guard.unlock();
		} break;
		default: {
			return kHelErrBadDescriptor;
		}
	}

	return kHelErrNone;
}

HelError helSubmitRecvString(HelHandle handle,
		HelHandle hub_handle, uint8_t *user_buffer, size_t max_length,
		int64_t filter_request, int64_t filter_sequence,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&universe->lock);
	auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
	if(!hub_wrapper)
		return kHelErrNoDescriptor;
	if(!(*hub_wrapper)->is<EventHubDescriptor>())
		return kHelErrBadDescriptor;
	auto event_hub = (*hub_wrapper)->get<EventHubDescriptor>().getEventHub();

	auto wrapper = universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	universe_guard.unlock();

	SubmitInfo submit_info(allocAsyncId(), submit_function, submit_object);
	
	switch((*wrapper)->tag()) {
		case AnyDescriptor::tagOf<BiDirectionFirstDescriptor>(): {
			auto &descriptor = (*wrapper)->get<BiDirectionFirstDescriptor>();
			KernelSharedPtr<BiDirectionPipe> pipe(descriptor.getPipe());
			Channel *channel = pipe->getFirstChannel();

			Channel::Guard channel_guard(&channel->lock);
			channel->submitRecvString(channel_guard, KernelSharedPtr<EventHub>(event_hub),
					user_buffer, max_length,
					filter_request, filter_sequence,
					submit_info);
			channel_guard.unlock();
		} break;
		case AnyDescriptor::tagOf<BiDirectionSecondDescriptor>(): {
			auto &descriptor = (*wrapper)->get<BiDirectionSecondDescriptor>();
			KernelSharedPtr<BiDirectionPipe> pipe(descriptor.getPipe());
			Channel *channel = pipe->getSecondChannel();

			Channel::Guard channel_guard(&channel->lock);
			channel->submitRecvString(channel_guard, KernelSharedPtr<EventHub>(event_hub),
					user_buffer, max_length,
					filter_request, filter_sequence,
					submit_info);
			channel_guard.unlock();
		} break;
		default: {
			return kHelErrBadDescriptor;
		}
	}

	*async_id = submit_info.asyncId;

	return kHelErrNone;
}

HelError helSubmitRecvDescriptor(HelHandle handle,
		HelHandle hub_handle,
		int64_t filter_request, int64_t filter_sequence,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&universe->lock);
	auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
	if(!hub_wrapper)
		return kHelErrNoDescriptor;
	if(!(*hub_wrapper)->is<EventHubDescriptor>())
		return kHelErrBadDescriptor;
	auto event_hub = (*hub_wrapper)->get<EventHubDescriptor>().getEventHub();

	auto wrapper = universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	universe_guard.unlock();

	SubmitInfo submit_info(allocAsyncId(), submit_function, submit_object);
	
	switch((*wrapper)->tag()) {
		case AnyDescriptor::tagOf<BiDirectionFirstDescriptor>(): {
			auto &descriptor = (*wrapper)->get<BiDirectionFirstDescriptor>();
			KernelSharedPtr<BiDirectionPipe> pipe(descriptor.getPipe());
			Channel *channel = pipe->getFirstChannel();

			Channel::Guard channel_guard(&channel->lock);
			channel->submitRecvDescriptor(channel_guard, KernelSharedPtr<EventHub>(event_hub),
					filter_request, filter_sequence, submit_info);
			channel_guard.unlock();
		} break;
		case AnyDescriptor::tagOf<BiDirectionSecondDescriptor>(): {
			auto &descriptor = (*wrapper)->get<BiDirectionSecondDescriptor>();
			KernelSharedPtr<BiDirectionPipe> pipe(descriptor.getPipe());
			Channel *channel = pipe->getSecondChannel();

			Channel::Guard channel_guard(&channel->lock);
			channel->submitRecvDescriptor(channel_guard, KernelSharedPtr<EventHub>(event_hub),
					filter_request, filter_sequence, submit_info);
			channel_guard.unlock();
		} break;
		default: {
			return kHelErrBadDescriptor;
		}
	}

	*async_id = submit_info.asyncId;

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
	
	Universe::Guard universe_guard(&universe->lock);
	auto serve_wrapper = universe->getDescriptor(universe_guard, handle);
	if(!serve_wrapper)
		return kHelErrNoDescriptor;
	if(!(*serve_wrapper)->is<ServerDescriptor>())
		return kHelErrBadDescriptor;
	auto &serve_desc = (*serve_wrapper)->get<ServerDescriptor>();
	KernelSharedPtr<Server> server(serve_desc.getServer());

	auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
	if(!hub_wrapper)
		return kHelErrNoDescriptor;
	if(!(*hub_wrapper)->is<EventHubDescriptor>())
		return kHelErrBadDescriptor;
	auto &hub_desc = (*hub_wrapper)->get<EventHubDescriptor>();
	KernelSharedPtr<EventHub> event_hub(hub_desc.getEventHub());
	universe_guard.unlock();
	
	SubmitInfo submit_info(allocAsyncId(), submit_function, submit_object);
	
	Server::Guard server_guard(&server->lock);
	server->submitAccept(server_guard, frigg::move(event_hub), submit_info);
	server_guard.unlock();

	*async_id = submit_info.asyncId;
	return kHelErrNone;
}

HelError helSubmitConnect(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&universe->lock);
	auto connect_wrapper = universe->getDescriptor(universe_guard, handle);
	if(!connect_wrapper)
		return kHelErrNoDescriptor;
	if(!(*connect_wrapper)->is<ClientDescriptor>())
		return kHelErrBadDescriptor;
	auto &connect_desc = (*connect_wrapper)->get<ClientDescriptor>();
	KernelSharedPtr<Server> server(connect_desc.getServer());

	auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
	if(!hub_wrapper)
		return kHelErrNoDescriptor;
	if(!(*hub_wrapper)->is<EventHubDescriptor>())
		return kHelErrBadDescriptor;
	auto &hub_desc = (*hub_wrapper)->get<EventHubDescriptor>();
	KernelSharedPtr<EventHub> event_hub(hub_desc.getEventHub());
	universe_guard.unlock();

	SubmitInfo submit_info(allocAsyncId(), submit_function, submit_object);
	
	Server::Guard server_guard(&server->lock);
	server->submitConnect(server_guard, frigg::move(event_hub), submit_info);
	server_guard.unlock();

	*async_id = submit_info.asyncId;
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
	if(!(*dir_wrapper)->is<RdDescriptor>())
		return kHelErrBadDescriptor;
	auto &dir_desc = (*dir_wrapper)->get<RdDescriptor>();
	KernelSharedPtr<RdFolder> directory(dir_desc.getFolder());

	auto mount_wrapper = universe->getDescriptor(universe_guard, mount_handle);
	if(!mount_wrapper)
		return kHelErrNoDescriptor;
	if(!(*mount_wrapper)->is<RdDescriptor>())
		return kHelErrBadDescriptor;
	auto &mount_desc = (*mount_wrapper)->get<RdDescriptor>();
	KernelSharedPtr<RdFolder> mount_directory(mount_desc.getFolder());
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
	if(!(*dir_wrapper)->is<RdDescriptor>())
		return kHelErrBadDescriptor;
	auto &dir_desc = (*dir_wrapper)->get<RdDescriptor>();
	KernelSharedPtr<RdFolder> directory(dir_desc.getFolder());
	
	// copy the descriptor we want to publish
	auto publish_wrapper = universe->getDescriptor(universe_guard, publish_handle);
	if(!publish_wrapper)
		return kHelErrNoDescriptor;
	AnyDescriptor publish_copy(**publish_wrapper);
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
				KernelSharedPtr<RdFolder> copy(directory);
			
				Universe::Guard universe_guard(&universe->lock);
				*handle = universe->attachDescriptor(universe_guard,
						RdDescriptor(frigg::move(copy)));
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

	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			IrqDescriptor(frigg::move(irq_line)));
	universe_guard.unlock();

	return kHelErrNone;
}
HelError helSubmitWaitForIrq(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&universe->lock);
	auto irq_wrapper = universe->getDescriptor(universe_guard, handle);
	if(!irq_wrapper)
		return kHelErrNoDescriptor;
	if(!(*irq_wrapper)->is<IrqDescriptor>())
		return kHelErrBadDescriptor;
	auto &irq_descriptor = (*irq_wrapper)->get<IrqDescriptor>();
	int number = irq_descriptor.getIrqLine()->getNumber();
	
	auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
	if(!hub_wrapper)
		return kHelErrNoDescriptor;
	if(!(*hub_wrapper)->is<EventHubDescriptor>())
		return kHelErrBadDescriptor;
	auto &hub_descriptor = (*hub_wrapper)->get<EventHubDescriptor>();
	KernelSharedPtr<EventHub> event_hub(hub_descriptor.getEventHub());
	universe_guard.unlock();

	SubmitInfo submit_info(allocAsyncId(), submit_function, submit_object);

	IrqRelay::Guard irq_guard(&irqRelays[number]->lock);
	irqRelays[number]->submitWaitRequest(irq_guard, frigg::move(event_hub), submit_info);
	irq_guard.unlock();

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
	
	Universe::Guard universe_guard(&universe->lock);
	auto wrapper = universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	if(!(*wrapper)->is<IoDescriptor>())
		return kHelErrBadDescriptor;
	auto &descriptor = (*wrapper)->get<IoDescriptor>();
	KernelSharedPtr<IoSpace> io_space(descriptor.getIoSpace());
	universe_guard.unlock();
	
	io_space->enableInThread(this_thread);

	return kHelErrNone;
}

HelError helEnableFullIo() {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();

	for(uintptr_t port = 0; port < 0x10000; port++)
		this_thread->enableIoPort(port);

	return kHelErrNone;
}

