
#include "../../frigg/include/types.hpp"
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
#include "../../hel/include/hel.h"

using namespace thor;

HelError helLog(const char *string, size_t length) {
	debug::criticalLogger->log(string, length);

	return kHelErrNone;
}


HelError helAllocateMemory(size_t size, HelHandle *handle) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();

	auto memory = makeShared<Memory>(kernelAlloc.get());
	memory->resize(size);
	
	MemoryAccessDescriptor base(util::move(memory));
	*handle = universe->attachDescriptor(util::move(base));

	return 0;
}

HelError helMapMemory(HelHandle memory_handle, void *pointer, size_t length) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	UnsafePtr<AddressSpace> address_space = this_thread->getAddressSpace();
	
	auto &wrapper = universe->getDescriptor(memory_handle);
	auto &descriptor = wrapper.get<MemoryAccessDescriptor>();
	UnsafePtr<Memory> memory = descriptor.getMemory();

	for(int offset = 0, i = 0; offset < length; offset += 0x1000, i++) {
		address_space->mapSingle4k((void *)((uintptr_t)pointer + offset),
				memory->getPage(i));
	}
	
	thorRtInvalidateSpace();
	
	return 0;
}


HelError helCreateThread(void (*user_entry) (uintptr_t), uintptr_t argument,
		void *user_stack_ptr, HelHandle *handle) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	UnsafePtr<AddressSpace> address_space = this_thread->getAddressSpace();

	auto new_thread = makeShared<Thread>(kernelAlloc.get());
	new_thread->setup(user_entry, argument, user_stack_ptr);
	new_thread->setUniverse(universe->shared<Universe>());
	new_thread->setAddressSpace(address_space->shared<AddressSpace>());

	scheduleQueue->addBack(util::move(new_thread));

//	ThreadObserveDescriptor base(util::move(new_thread));
//	*handle = universe->attachDescriptor(util::move(base));

	return 0;
}

HelError helExitThisThread() {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	scheduleQueue->remove(this_thread);
}


HelError helCreateEventHub(HelHandle *handle) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto event_hub = makeShared<EventHub>(kernelAlloc.get());

	EventHubDescriptor base(util::move(event_hub));

	*handle = universe->attachDescriptor(util::move(base));

	return 0;
}

HelError helWaitForEvents(HelHandle handle,
		HelEvent *user_list, size_t max_items,
		HelNanotime max_time, size_t *num_items) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
	AnyDescriptor &hub_wrapper = universe->getDescriptor(handle);
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	UnsafePtr<EventHub> event_hub = hub_descriptor.getEventHub();

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
				debug::criticalLogger->log("Unexpected error");
				debug::panic();
			}
		} break;
		case EventHub::Event::kTypeAccept: {
			user_evt->type = kHelEventAccept;

			BiDirectionFirstDescriptor descriptor(util::move(event.pipe));
			user_evt->handle = universe->attachDescriptor(util::move(descriptor));
		} break;
		case EventHub::Event::kTypeConnect: {
			user_evt->type = kHelEventConnect;

			BiDirectionSecondDescriptor descriptor(util::move(event.pipe));
			user_evt->handle = universe->attachDescriptor(util::move(descriptor));
		} break;
		case EventHub::Event::kTypeIrq: {
			user_evt->type = kHelEventIrq;
		} break;
		default:
			debug::criticalLogger->log("Illegal event type");
			debug::panic();
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
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto pipe = makeShared<BiDirectionPipe>(kernelAlloc.get());

	BiDirectionFirstDescriptor first_base(pipe->shared<BiDirectionPipe>());
	BiDirectionSecondDescriptor second_base(pipe->shared<BiDirectionPipe>());

	*first_handle = universe->attachDescriptor(util::move(first_base));
	*second_handle = universe->attachDescriptor(util::move(second_base));

	return 0;
}

HelError helSendString(HelHandle handle,
		const uint8_t *user_buffer, size_t length,
		int64_t msg_request, int64_t msg_sequence) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
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
			debug::criticalLogger->log("Descriptor is not a source");
			debug::panic();
		}
	}

	return 0;
}

HelError helSubmitRecvString(HelHandle handle,
		HelHandle hub_handle, uint8_t *user_buffer, size_t max_length,
		int64_t filter_request, int64_t filter_sequence,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
	AnyDescriptor &hub_wrapper = universe->getDescriptor(hub_handle);
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	
	auto event_hub = hub_descriptor.getEventHub()->shared<EventHub>();
	SubmitInfo submit_info(submit_id, submit_function, submit_object);
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	switch(wrapper.tag()) {
		case AnyDescriptor::tagOf<BiDirectionFirstDescriptor>(): {
			auto &descriptor = wrapper.get<BiDirectionFirstDescriptor>();
			Channel *channel = descriptor.getPipe()->getFirstChannel();
			channel->submitRecvString(util::move(event_hub),
					user_buffer, max_length,
					filter_request, filter_sequence,
					submit_info);
		} break;
		case AnyDescriptor::tagOf<BiDirectionSecondDescriptor>(): {
			auto &descriptor = wrapper.get<BiDirectionSecondDescriptor>();
			Channel *channel = descriptor.getPipe()->getSecondChannel();
			channel->submitRecvString(util::move(event_hub),
					user_buffer, max_length,
					filter_request, filter_sequence,
					submit_info);
		} break;
		default: {
			debug::criticalLogger->log("Descriptor is not a source");
			debug::panic();
		}
	}

	return 0;
}


HelError helCreateServer(HelHandle *server_handle, HelHandle *client_handle) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto server = makeShared<Server>(kernelAlloc.get());

	ServerDescriptor server_descriptor(server->shared<Server>());
	ClientDescriptor client_descriptor(server->shared<Server>());

	*server_handle = universe->attachDescriptor(util::move(server_descriptor));
	*client_handle = universe->attachDescriptor(util::move(client_descriptor));

	return 0;
}

HelError helSubmitAccept(HelHandle handle, HelHandle hub_handle,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	AnyDescriptor &hub_wrapper = universe->getDescriptor(hub_handle);
	auto &descriptor = wrapper.get<ServerDescriptor>();
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	
	auto event_hub = hub_descriptor.getEventHub()->shared<EventHub>();
	SubmitInfo submit_info(submit_id, submit_function, submit_object);
	
	descriptor.getServer()->submitAccept(util::move(event_hub), submit_info);
	
	return 0;
}

HelError helSubmitConnect(HelHandle handle, HelHandle hub_handle,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	AnyDescriptor &hub_wrapper = universe->getDescriptor(hub_handle);
	auto &descriptor = wrapper.get<ClientDescriptor>();
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	
	auto event_hub = hub_descriptor.getEventHub()->shared<EventHub>();
	SubmitInfo submit_info(submit_id, submit_function, submit_object);
	
	descriptor.getServer()->submitConnect(util::move(event_hub), submit_info);
	
	return 0;
}


HelError helAccessIrq(int number, HelHandle *handle) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto irq_line = makeShared<IrqLine>(kernelAlloc.get(), number);

	IrqDescriptor base(util::move(irq_line));

	*handle = universe->attachDescriptor(util::move(base));

	return 0;
}
HelError helSubmitWaitForIrq(HelHandle handle, HelHandle hub_handle,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
	AnyDescriptor &irq_wrapper = universe->getDescriptor(handle);
	AnyDescriptor &hub_wrapper = universe->getDescriptor(hub_handle);
	auto &irq_descriptor = irq_wrapper.get<IrqDescriptor>();
	auto &hub_descriptor = hub_wrapper.get<EventHubDescriptor>();
	
	int number = irq_descriptor.getIrqLine()->getNumber();

	auto event_hub = hub_descriptor.getEventHub()->shared<EventHub>();
	SubmitInfo submit_info(submit_id, submit_function, submit_object);
	
	(*irqRelays)[number].submitWaitRequest(util::move(event_hub),
			submit_info);
}

HelError helAccessIo(uintptr_t *user_port_array, size_t num_ports,
		HelHandle *handle) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights
	auto io_space = makeShared<IoSpace>(kernelAlloc.get());
	for(size_t i = 0; i < num_ports; i++)
		io_space->addPort(user_port_array[i]);

	IoDescriptor base(util::move(io_space));
	*handle = universe->attachDescriptor(util::move(base));
}
HelError helEnableIo(HelHandle handle) {
	UnsafePtr<Thread> this_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> universe = this_thread->getUniverse();
	
	AnyDescriptor &wrapper = universe->getDescriptor(handle);
	auto &descriptor = wrapper.get<IoDescriptor>();
	
	descriptor.getIoSpace()->enableInThread(this_thread);
}

