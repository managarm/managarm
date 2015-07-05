
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
#include "../../hel/include/hel.h"

using namespace thor;

HelError helLog(const char *string, size_t length) {
	debug::criticalLogger->log(string, length);

	return kHelErrNone;
}


HelError helAllocateMemory(size_t size, HelHandle *handle) {
	UnsafePtr<Thread> cur_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> cur_universe = cur_thread->getUniverse();

	auto memory = makeShared<Memory>(kernelAlloc.get());
	memory->resize(size);
	
	MemoryAccessDescriptor base(util::move(memory));
	*handle = cur_universe->attachDescriptor(util::move(base));

	return 0;
}

HelError helMapMemory(HelHandle memory_handle, void *pointer, size_t length) {
	UnsafePtr<Thread> cur_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> cur_universe = cur_thread->getUniverse();
	UnsafePtr<AddressSpace> address_space = cur_thread->getAddressSpace();
	
	auto &descriptor = cur_universe->getDescriptor(memory_handle).asMemoryAccess();
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
	UnsafePtr<Thread> cur_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> cur_universe = cur_thread->getUniverse();
	UnsafePtr<AddressSpace> address_space = cur_thread->getAddressSpace();

	auto new_thread = makeShared<Thread>(kernelAlloc.get());
	new_thread->setup(user_entry, argument, user_stack_ptr);
	new_thread->setUniverse(cur_universe->shared<Universe>());
	new_thread->setAddressSpace(address_space->shared<AddressSpace>());

	debug::criticalLogger->log("x");
	scheduleQueue->addBack(util::move(new_thread));

//	ThreadObserveDescriptor base(util::move(new_thread));
//	*handle = cur_universe->attachDescriptor(util::move(base));

	return 0;
}


HelError helCreateBiDirectionPipe(HelHandle *first_handle,
		HelHandle *second_handle) {
	UnsafePtr<Thread> cur_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> cur_universe = cur_thread->getUniverse();
	
	auto pipe = makeShared<BiDirectionPipe>(kernelAlloc.get());

	BiDirectionFirstDescriptor first_base(pipe->shared<BiDirectionPipe>());
	BiDirectionSecondDescriptor second_base(pipe->shared<BiDirectionPipe>());

	*first_handle = cur_universe->attachDescriptor(util::move(first_base));
	*second_handle = cur_universe->attachDescriptor(util::move(second_base));

	return 0;
}

HelError helRecvString(HelHandle handle, char *buffer, size_t length) {
	UnsafePtr<Thread> cur_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> cur_universe = cur_thread->getUniverse();
	
	AnyDescriptor &any_desc = cur_universe->getDescriptor(handle);
	switch(any_desc.getType()) {
		case AnyDescriptor::kTypeBiDirectionFirst: {
			auto &descriptor = any_desc.asBiDirectionFirst();
			descriptor.recvString(buffer, length);
		} break;
		case AnyDescriptor::kTypeBiDirectionSecond: {
			auto &descriptor = any_desc.asBiDirectionSecond();
			descriptor.recvString(buffer, length);
		} break;
		default: {
			debug::criticalLogger->log("Descriptor is not a source");
			debug::panic();
		}
	}

	return 0;
}

HelError helSendString(HelHandle handle, const char *buffer, size_t length) {
	UnsafePtr<Thread> cur_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> cur_universe = cur_thread->getUniverse();
	
	AnyDescriptor &any_desc = cur_universe->getDescriptor(handle);
	switch(any_desc.getType()) {
		case AnyDescriptor::kTypeBiDirectionFirst: {
			auto &descriptor = any_desc.asBiDirectionFirst();
			descriptor.sendString(buffer, length);
		} break;
		case AnyDescriptor::kTypeBiDirectionSecond: {
			auto &descriptor = any_desc.asBiDirectionSecond();
			descriptor.sendString(buffer, length);
		} break;
		default: {
			debug::criticalLogger->log("Descriptor is not a source");
			debug::panic();
		}
	}

	return 0;
}

// --------------------------------------------------------
// FIXME
// --------------------------------------------------------

void helSwitchThread(HelHandle thread_handle) {
/*	UnsafePtr<Thread> cur_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Universe> cur_universe = cur_thread->getUniverse();
	
	auto thread_descriptor = (Thread::ThreadDescriptor *)cur_universe->getDescriptor(thread_handle);
	UnsafePtr<Thread> thread = thread_descriptor->getThread();

	thread->switchTo();*/
}

