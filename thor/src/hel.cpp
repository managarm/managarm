
#include "kernel.hpp"
#include "../../hel/include/hel.h"

using namespace thor;
namespace traits = frigg::traits;
namespace debug = frigg::debug;
namespace util = frigg::util;

HelError helLog(const char *string, size_t length) {
	for(size_t i = 0; i < length; i++)
		infoSink.print(string[i]);

	return kHelErrNone;
}


HelError helCloseDescriptor(HelHandle handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	universe->detachDescriptor(handle);

	return kHelErrNone;
}


HelError helAllocateMemory(size_t size, HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();

	auto memory = makeShared<Memory>(*kernelAlloc);
	memory->resize(size);
	
	MemoryAccessDescriptor base(traits::move(memory));
	*handle = universe->attachDescriptor(traits::move(base));

	return 0;
}

HelError helAccessPhysical(uintptr_t physical, size_t size, HelHandle *handle) {
	ASSERT((physical % kPageSize) == 0);
	ASSERT((size % kPageSize) == 0);

	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	auto memory = makeShared<Memory>(*kernelAlloc);
	for(size_t offset = 0; offset < size; offset += kPageSize)
		memory->addPage(physical + offset);
	
	MemoryAccessDescriptor base(traits::move(memory));
	*handle = universe->attachDescriptor(traits::move(base));

	return 0;
}

HelError helCreateSpace(HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();

	auto space = makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());
	
	AddressSpaceDescriptor base(traits::move(space));
	*handle = universe->attachDescriptor(traits::move(base));

	return 0;
}

HelError helMapMemory(HelHandle memory_handle, HelHandle space_handle,
		void *pointer, size_t length, uint32_t flags, void **actual_pointer) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	UnsafePtr<AddressSpace, KernelAlloc> space;
	if(space_handle == kHelNullHandle) {
		space = this_thread->getAddressSpace();
	}else{
		auto &space_wrapper = universe->getDescriptor(space_handle);
		space = space_wrapper.get<AddressSpaceDescriptor>().getSpace();
	}
	
	auto &wrapper = universe->getDescriptor(memory_handle);
	auto &descriptor = wrapper.get<MemoryAccessDescriptor>();
	UnsafePtr<Memory, KernelAlloc> memory = descriptor.getMemory();

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
		ASSERT((flags & mask) == kHelMapReadOnly);
		map_flags |= AddressSpace::kMapReadOnly;
	}
	
	VirtualAddr actual_address;
	space->map(memory, (uintptr_t)pointer, length, map_flags, &actual_address);
	thorRtInvalidateSpace();

	*actual_pointer = (void *)actual_address;

	return 0;
}

HelError helMemoryInfo(HelHandle handle, size_t *size) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	auto &wrapper = universe->getDescriptor(handle);
	auto &descriptor = wrapper.get<MemoryAccessDescriptor>();
	UnsafePtr<Memory, KernelAlloc> memory = descriptor.getMemory();

	*size = memory->getSize();

	return kHelErrNone;
}


HelError helCreateThread(HelHandle space_handle,
		HelHandle directory_handle, HelThreadState *user_state, HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> this_universe = this_thread->getUniverse();
	
	UnsafePtr<AddressSpace, KernelAlloc> address_space;
	if(space_handle == kHelNullHandle) {
		address_space = this_thread->getAddressSpace();
	}else{
		auto &space_wrapper = this_universe->getDescriptor(space_handle);
		address_space = space_wrapper.get<AddressSpaceDescriptor>().getSpace();
	}

	UnsafePtr<RdFolder, KernelAlloc> directory;
	if(directory_handle == kHelNullHandle) {
		directory = this_thread->getDirectory();
	}else{
		auto &directory_wrapper = this_universe->getDescriptor(directory_handle);
		directory = directory_wrapper.get<RdDescriptor>().getFolder();
	}

	auto new_thread = makeShared<Thread>(*kernelAlloc,
			SharedPtr<Universe, KernelAlloc>(this_universe),
			SharedPtr<AddressSpace, KernelAlloc>(address_space),
			SharedPtr<RdFolder, KernelAlloc>(directory), false);
	
	ThorRtGeneralState &state = new_thread->accessState().generalState;

	state.rax = user_state->rax;
	state.rbx = user_state->rbx;
	state.rcx = user_state->rcx;
	state.rdx = user_state->rdx;
	state.rsi = user_state->rsi;
	state.rdi = user_state->rdi;
	state.rbp = user_state->rbp;

	state.r8 = user_state->r8;
	state.r9 = user_state->r9;
	state.r10 = user_state->r10;
	state.r11 = user_state->r11;
	state.r12 = user_state->r12;
	state.r13 = user_state->r13;
	state.r14 = user_state->r14;
	state.r15 = user_state->r15;

	state.rip = user_state->rip;
	state.rsp = user_state->rsp;
	state.rflags = 0x200; // set the interrupt flag

	scheduleQueue->addBack(traits::move(new_thread));

//	ThreadObserveDescriptor base(traits::move(new_thread));
//	*handle = universe->attachDescriptor(traits::move(base));

	return 0;
}

HelError helExitThisThread() {
	// schedule without re-enqueuing this thread first
	doSchedule();

	return kHelErrNone;
}


HelError helCreateEventHub(HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	auto event_hub = makeShared<EventHub>(*kernelAlloc);

	EventHubDescriptor base(traits::move(event_hub));

	*handle = universe->attachDescriptor(traits::move(base));

	return 0;
}

HelError helWaitForEvents(HelHandle handle,
		HelEvent *user_list, size_t max_items,
		HelNanotime max_time, size_t *num_items) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	AnyDescriptor &hub_wrapper = universe->getDescriptor(handle);
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	UnsafePtr<EventHub, KernelAlloc> event_hub = hub_descriptor.getEventHub();

	// TODO: check userspace page access rights

	size_t count; 
	for(count = 0; count < max_items; count++) {
		if(!event_hub->hasEvent())
			break;
		EventHub::Event event = event_hub->dequeueEvent();

		HelEvent *user_evt = &user_list[count];
		switch(event.type) {
		case EventHub::Event::kTypeRecvStringTransfer: {
			user_evt->type = kHelEventRecvString;
			user_evt->error = kHelErrNone;

			// TODO: check userspace page access rights
	
			// do the actual memory transfer
			memcpy(event.userBuffer, event.kernelBuffer, event.length);
			user_evt->length = event.length;
		} break;
		case EventHub::Event::kTypeRecvStringError: {
			user_evt->type = kHelEventRecvString;

			switch(event.error) {
			case kErrBufferTooSmall:
				user_evt->error = kHelErrBufferTooSmall;
				break;
			default:
				ASSERT(!"Unexpected error");
			}
		} break;
		case EventHub::Event::kTypeRecvDescriptor: {
			user_evt->type = kHelEventRecvDescriptor;
			
			AnyDescriptor wrapper = traits::move(event.descriptor);
			user_evt->handle = universe->attachDescriptor(traits::move(wrapper));
		} break;
		case EventHub::Event::kTypeAccept: {
			user_evt->type = kHelEventAccept;

			BiDirectionFirstDescriptor descriptor(traits::move(event.pipe));
			user_evt->handle = universe->attachDescriptor(traits::move(descriptor));
		} break;
		case EventHub::Event::kTypeConnect: {
			user_evt->type = kHelEventConnect;

			BiDirectionSecondDescriptor descriptor(traits::move(event.pipe));
			user_evt->handle = universe->attachDescriptor(traits::move(descriptor));
		} break;
		case EventHub::Event::kTypeIrq: {
			user_evt->type = kHelEventIrq;
		} break;
		default:
			ASSERT(!"Illegal event type");
		}

		user_evt->submitId = event.submitInfo.submitId;
		user_evt->submitFunction = event.submitInfo.submitFunction;
		user_evt->submitObject = event.submitInfo.submitObject;
	}
	*num_items = count;

	return 0;
}


HelError helCreateBiDirectionPipe(HelHandle *first_handle,
		HelHandle *second_handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	auto pipe = makeShared<BiDirectionPipe>(*kernelAlloc);
	SharedPtr<BiDirectionPipe, KernelAlloc> copy(pipe);

	BiDirectionFirstDescriptor first_base(traits::move(pipe));
	BiDirectionSecondDescriptor second_base(traits::move(copy));
	
	*first_handle = universe->attachDescriptor(traits::move(first_base));
	*second_handle = universe->attachDescriptor(traits::move(second_base));

	return 0;
}

HelError helSendString(HelHandle handle,
		const uint8_t *user_buffer, size_t length,
		int64_t msg_request, int64_t msg_sequence) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	switch(wrapper.tag()) {
		case AnyDescriptor::tagOf<BiDirectionFirstDescriptor>(): {
			auto &descriptor = wrapper.get<BiDirectionFirstDescriptor>();
			Channel *channel = descriptor.getPipe()->getSecondChannel();
			channel->sendString(user_buffer, length,
					msg_request, msg_sequence);
		} break;
		case AnyDescriptor::tagOf<BiDirectionSecondDescriptor>(): {
			auto &descriptor = wrapper.get<BiDirectionSecondDescriptor>();
			Channel *channel = descriptor.getPipe()->getFirstChannel();
			channel->sendString(user_buffer, length,
					msg_request, msg_sequence);
		} break;
		default: {
			ASSERT(!"Descriptor is not a sink");
		}
	}

	return 0;
}

HelError helSendDescriptor(HelHandle handle, HelHandle send_handle,
		int64_t msg_request, int64_t msg_sequence) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights

	AnyDescriptor &send_wrapper = universe->getDescriptor(send_handle);
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	switch(wrapper.tag()) {
		case AnyDescriptor::tagOf<BiDirectionFirstDescriptor>(): {
			auto &descriptor = wrapper.get<BiDirectionFirstDescriptor>();
			Channel *channel = descriptor.getPipe()->getSecondChannel();
			channel->sendDescriptor(AnyDescriptor(send_wrapper),
					msg_request, msg_sequence);
		} break;
		case AnyDescriptor::tagOf<BiDirectionSecondDescriptor>(): {
			auto &descriptor = wrapper.get<BiDirectionSecondDescriptor>();
			Channel *channel = descriptor.getPipe()->getFirstChannel();
			channel->sendDescriptor(AnyDescriptor(send_wrapper),
					msg_request, msg_sequence);
		} break;
		default: {
			ASSERT(!"Descriptor is not a sink");
		}
	}

	return 0;
}

HelError helSubmitRecvString(HelHandle handle,
		HelHandle hub_handle, uint8_t *user_buffer, size_t max_length,
		int64_t filter_request, int64_t filter_sequence,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	AnyDescriptor &hub_wrapper = universe->getDescriptor(hub_handle);
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	
	auto event_hub = hub_descriptor.getEventHub();
	SubmitInfo submit_info(submit_id, submit_function, submit_object);
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	switch(wrapper.tag()) {
		case AnyDescriptor::tagOf<BiDirectionFirstDescriptor>(): {
			auto &descriptor = wrapper.get<BiDirectionFirstDescriptor>();
			Channel *channel = descriptor.getPipe()->getFirstChannel();
			channel->submitRecvString(SharedPtr<EventHub, KernelAlloc>(event_hub),
					user_buffer, max_length,
					filter_request, filter_sequence,
					submit_info);
		} break;
		case AnyDescriptor::tagOf<BiDirectionSecondDescriptor>(): {
			auto &descriptor = wrapper.get<BiDirectionSecondDescriptor>();
			Channel *channel = descriptor.getPipe()->getSecondChannel();
			channel->submitRecvString(SharedPtr<EventHub, KernelAlloc>(event_hub),
					user_buffer, max_length,
					filter_request, filter_sequence,
					submit_info);
		} break;
		default: {
			ASSERT(!"Descriptor is not a source");
		}
	}

	return 0;
}

HelError helSubmitRecvDescriptor(HelHandle handle,
		HelHandle hub_handle,
		int64_t filter_request, int64_t filter_sequence,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	AnyDescriptor &hub_wrapper = universe->getDescriptor(hub_handle);
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	
	auto event_hub = hub_descriptor.getEventHub();
	SubmitInfo submit_info(submit_id, submit_function, submit_object);
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	switch(wrapper.tag()) {
		case AnyDescriptor::tagOf<BiDirectionFirstDescriptor>(): {
			auto &descriptor = wrapper.get<BiDirectionFirstDescriptor>();
			Channel *channel = descriptor.getPipe()->getFirstChannel();
			channel->submitRecvDescriptor(SharedPtr<EventHub, KernelAlloc>(event_hub),
					filter_request, filter_sequence, submit_info);
		} break;
		case AnyDescriptor::tagOf<BiDirectionSecondDescriptor>(): {
			auto &descriptor = wrapper.get<BiDirectionSecondDescriptor>();
			Channel *channel = descriptor.getPipe()->getSecondChannel();
			channel->submitRecvDescriptor(SharedPtr<EventHub, KernelAlloc>(event_hub),
					filter_request, filter_sequence, submit_info);
		} break;
		default: {
			ASSERT(!"Descriptor is not a source");
		}
	}

	return 0;
}


HelError helCreateServer(HelHandle *server_handle, HelHandle *client_handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	auto server = makeShared<Server>(*kernelAlloc);
	SharedPtr<Server, KernelAlloc> copy(server);

	ServerDescriptor server_descriptor(traits::move(server));
	ClientDescriptor client_descriptor(traits::move(copy));

	*server_handle = universe->attachDescriptor(traits::move(server_descriptor));
	*client_handle = universe->attachDescriptor(traits::move(client_descriptor));

	return 0;
}

HelError helSubmitAccept(HelHandle handle, HelHandle hub_handle,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	AnyDescriptor &hub_wrapper = universe->getDescriptor(hub_handle);
	auto &descriptor = wrapper.get<ServerDescriptor>();
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	
	auto event_hub = hub_descriptor.getEventHub();
	SubmitInfo submit_info(submit_id, submit_function, submit_object);
	
	descriptor.getServer()->submitAccept(SharedPtr<EventHub, KernelAlloc>(event_hub), submit_info);
	
	return 0;
}

HelError helSubmitConnect(HelHandle handle, HelHandle hub_handle,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	AnyDescriptor &hub_wrapper = universe->getDescriptor(hub_handle);
	auto &descriptor = wrapper.get<ClientDescriptor>();
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	
	auto event_hub = hub_descriptor.getEventHub();
	SubmitInfo submit_info(submit_id, submit_function, submit_object);
	
	descriptor.getServer()->submitConnect(SharedPtr<EventHub, KernelAlloc>(event_hub), submit_info);
	
	return 0;
}


HelError helCreateRd(HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	auto folder = makeShared<RdFolder>(*kernelAlloc);

	RdDescriptor base(traits::move(folder));
	*handle = universe->attachDescriptor(traits::move(base));
	
	return kHelErrNone;
}

HelError helRdMount(HelHandle handle, const char *user_name,
		size_t name_length, HelHandle mount_handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();

	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	AnyDescriptor &mount_wrapper = universe->getDescriptor(mount_handle);

	UnsafePtr<RdFolder, KernelAlloc> directory
			= wrapper.get<RdDescriptor>().getFolder();
	UnsafePtr<RdFolder, KernelAlloc> mount_directory
			= mount_wrapper.get<RdDescriptor>().getFolder();
	directory->mount(user_name, name_length,
			SharedPtr<RdFolder, KernelAlloc>(mount_directory));
	
	return kHelErrNone;
}

HelError helRdPublish(HelHandle handle, const char *user_name,
		size_t name_length, HelHandle publish_handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();

	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	AnyDescriptor &publish_wrapper = universe->getDescriptor(publish_handle);

	auto &descriptor = wrapper.get<RdDescriptor>();
	UnsafePtr<RdFolder, KernelAlloc> folder = descriptor.getFolder();

	AnyDescriptor publish_copy(publish_wrapper);
	folder->publish(user_name, name_length, traits::move(publish_copy));
	
	return kHelErrNone;
}

HelError helRdOpen(const char *user_name, size_t name_length, HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();

	// TODO: verifiy access rights for user_name
	
	auto find_char = [] (const char *string, char c,
			size_t start_at, size_t max_length) -> size_t {
		for(size_t i = start_at; i < max_length; i++)
			if(string[i] == c)
				return i;
		return max_length;
	};
	
	UnsafePtr<RdFolder, KernelAlloc> directory = this_thread->getDirectory();
	
	size_t search_from = 0;
	while(true) {
		size_t next_slash = find_char(user_name, '/', search_from, name_length);
		util::StringView part(user_name + search_from, next_slash - search_from);
		if(next_slash == name_length) {
			if(part == util::StringView("#this")) {
				// open a handle to this directory
				SharedPtr<RdFolder, KernelAlloc> copy(directory);
				RdDescriptor descriptor(traits::move(copy));
				*handle = universe->attachDescriptor(traits::move(descriptor));

				return kHelErrNone;
			}else{
				// read a file from this directory
				RdFolder::Entry *entry = directory->getEntry(part.data(), part.size());
				ASSERT(entry != nullptr);

				AnyDescriptor copy(entry->descriptor);
				*handle = universe->attachDescriptor(traits::move(copy));

				return kHelErrNone;
			}
		}else{
			// read a subdirectory of this directory
			RdFolder::Entry *entry = directory->getEntry(part.data(), part.size());
			ASSERT(entry != nullptr);

			directory = entry->mounted;
		}
		search_from = next_slash + 1;
	}
}


HelError helAccessIrq(int number, HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	auto irq_line = makeShared<IrqLine>(*kernelAlloc, number);

	IrqDescriptor base(traits::move(irq_line));

	*handle = universe->attachDescriptor(traits::move(base));

	return 0;
}
HelError helSubmitWaitForIrq(HelHandle handle, HelHandle hub_handle,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	AnyDescriptor &irq_wrapper = universe->getDescriptor(handle);
	AnyDescriptor &hub_wrapper = universe->getDescriptor(hub_handle);
	auto &irq_descriptor = irq_wrapper.get<IrqDescriptor>();
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	
	int number = irq_descriptor.getIrqLine()->getNumber();

	auto event_hub = hub_descriptor.getEventHub();
	SubmitInfo submit_info(submit_id, submit_function, submit_object);
	
	irqRelays[number]->submitWaitRequest(SharedPtr<EventHub, KernelAlloc>(event_hub),
			submit_info);

	return kHelErrNone;
}

HelError helAccessIo(uintptr_t *user_port_array, size_t num_ports,
		HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights
	auto io_space = makeShared<IoSpace>(*kernelAlloc);
	for(size_t i = 0; i < num_ports; i++)
		io_space->addPort(user_port_array[i]);

	IoDescriptor base(traits::move(io_space));
	*handle = universe->attachDescriptor(traits::move(base));

	return kHelErrNone;
}
HelError helEnableIo(HelHandle handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = getCurrentThread();
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	auto &descriptor = wrapper.get<IoDescriptor>();
	
	descriptor.getIoSpace()->enableInThread(this_thread);

	return kHelErrNone;
}

