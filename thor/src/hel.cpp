
#include "kernel.hpp"
#include "../../hel/include/hel.h"

using namespace thor;
namespace traits = frigg::traits;
namespace debug = frigg::debug;

HelError helLog(const char *string, size_t length) {
	for(size_t i = 0; i < length; i++)
		infoSink.print(string[i]);

	return kHelErrNone;
}


HelError helCloseDescriptor(HelHandle handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	universe->detachDescriptor(handle);

	return kHelErrNone;
}


HelError helAllocateMemory(size_t size, HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();

	auto memory = makeShared<Memory>(*kernelAlloc);
	memory->resize(size);
	
	MemoryAccessDescriptor base(traits::move(memory));
	*handle = universe->attachDescriptor(traits::move(base));

	return 0;
}

HelError helCreateSpace(HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();

	auto space = makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());
	
	AddressSpaceDescriptor base(traits::move(space));
	*handle = universe->attachDescriptor(traits::move(base));

	return 0;
}

HelError helMapMemory(HelHandle memory_handle, HelHandle space_handle,
		void *pointer, size_t length, uint32_t flags, void **actual_pointer) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	UnsafePtr<AddressSpace, KernelAlloc> space;
	if(space_handle != kHelNullHandle) {
		auto &space_wrapper = universe->getDescriptor(space_handle);
		space = space_wrapper.get<AddressSpaceDescriptor>().getSpace();
	}else{
		space = this_thread->getAddressSpace();
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
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	auto &wrapper = universe->getDescriptor(handle);
	auto &descriptor = wrapper.get<MemoryAccessDescriptor>();
	UnsafePtr<Memory, KernelAlloc> memory = descriptor.getMemory();

	*size = memory->getSize();

	return kHelErrNone;
}


HelError helCreateThread(void (*user_entry) (uintptr_t), uintptr_t argument,
		void *user_stack_ptr, HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	UnsafePtr<AddressSpace, KernelAlloc> address_space = this_thread->getAddressSpace();
	UnsafePtr<RdFolder, KernelAlloc> directory = this_thread->getDirectory();

	auto new_thread = makeShared<Thread>(*kernelAlloc,
			SharedPtr<Universe, KernelAlloc>(universe),
			SharedPtr<AddressSpace, KernelAlloc>(address_space),
			SharedPtr<RdFolder, KernelAlloc>(directory), false);

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
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	auto event_hub = makeShared<EventHub>(*kernelAlloc);

	EventHubDescriptor base(traits::move(event_hub));

	*handle = universe->attachDescriptor(traits::move(base));

	return 0;
}

HelError helWaitForEvents(HelHandle handle,
		HelEvent *user_list, size_t max_items,
		HelNanotime max_time, size_t *num_items) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
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
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
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
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
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

HelError helSubmitRecvString(HelHandle handle,
		HelHandle hub_handle, uint8_t *user_buffer, size_t max_length,
		int64_t filter_request, int64_t filter_sequence,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
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


HelError helCreateServer(HelHandle *server_handle, HelHandle *client_handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
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
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
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
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
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
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	auto folder = makeShared<RdFolder>(*kernelAlloc);

	RdDescriptor base(traits::move(folder));
	*handle = universe->attachDescriptor(traits::move(base));
	
	return kHelErrNone;
}
HelError helRdPublish(HelHandle handle, const char *user_name,
		size_t name_length, HelHandle publish_handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();

	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	AnyDescriptor &publish_wrapper = universe->getDescriptor(publish_handle);

	auto &descriptor = wrapper.get<RdDescriptor>();
	UnsafePtr<RdFolder, KernelAlloc> folder = descriptor.getFolder();

	AnyDescriptor publish_copy(publish_wrapper);
	folder->publish(user_name, name_length, traits::move(publish_copy));
	
	return kHelErrNone;
}
HelError helRdOpen(const char *user_name, size_t name_length,
		HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	UnsafePtr<RdFolder, KernelAlloc> directory = this_thread->getDirectory();
	RdFolder::Entry *entry = directory->getEntry(user_name, name_length);
	ASSERT(entry != nullptr);
	
	AnyDescriptor copy(entry->descriptor);
	*handle = universe->attachDescriptor(traits::move(copy));

	return kHelErrNone;
}


HelError helAccessIrq(int number, HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	auto irq_line = makeShared<IrqLine>(*kernelAlloc, number);

	IrqDescriptor base(traits::move(irq_line));

	*handle = universe->attachDescriptor(traits::move(base));

	return 0;
}
HelError helSubmitWaitForIrq(HelHandle handle, HelHandle hub_handle,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	AnyDescriptor &irq_wrapper = universe->getDescriptor(handle);
	AnyDescriptor &hub_wrapper = universe->getDescriptor(hub_handle);
	auto &irq_descriptor = irq_wrapper.get<IrqDescriptor>();
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	
	int number = irq_descriptor.getIrqLine()->getNumber();

	auto event_hub = hub_descriptor.getEventHub();
	SubmitInfo submit_info(submit_id, submit_function, submit_object);
	
	(*irqRelays)[number].submitWaitRequest(SharedPtr<EventHub, KernelAlloc>(event_hub),
			submit_info);

	return kHelErrNone;
}

HelError helAccessIo(uintptr_t *user_port_array, size_t num_ports,
		HelHandle *handle) {
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
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
	UnsafePtr<Thread, KernelAlloc> this_thread = *currentThread;
	UnsafePtr<Universe, KernelAlloc> universe = this_thread->getUniverse();
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	auto &descriptor = wrapper.get<IoDescriptor>();
	
	descriptor.getIoSpace()->enableInThread(this_thread);

	return kHelErrNone;
}

