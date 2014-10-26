
#include "../../frigg/include/arch_x86/types64.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "util/vector.hpp"
#include "util/smart-ptr.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"
#include "core.hpp"
#include "../../hel/include/hel.h"

using namespace thor;

HelDescriptor helCreateMemory(size_t length) {
	UnsafePtr<Thread> cur_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Process> cur_process = cur_thread->getProcess();

	auto memory = makeShared<Memory>(memory::kernelAllocator.access());
	memory->resize(length);
	
	auto descriptor = new (memory::kernelAllocator.access()) Memory::AccessDescriptor(memory->unsafe<Memory>());
	cur_process->attachDescriptor(descriptor);
	return descriptor->getHandle();
}

void helMapMemory(HelDescriptor memory_handle, void *pointer, size_t length) {
	UnsafePtr<Thread> cur_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Process> cur_process = cur_thread->getProcess();
	UnsafePtr<AddressSpace> address_space = cur_thread->getAddressSpace();
	
	auto access_descriptor = (Memory::AccessDescriptor *)cur_process->getDescriptor(memory_handle);
	UnsafePtr<Memory> memory = access_descriptor->getMemory();

	for(int offset = 0, i = 0; offset < length; offset += 0x1000, i++) {
		address_space->mapSingle4k((void *)((uintptr_t)pointer + offset),
				memory->getPage(i));
	}
	
	thorRtInvalidateSpace();
}

HelDescriptor helCreateThread(void *entry) {
	UnsafePtr<Thread> cur_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Process> cur_process = cur_thread->getProcess();
	UnsafePtr<AddressSpace> address_space = cur_thread->getAddressSpace();

	auto new_thread = makeShared<Thread>(memory::kernelAllocator.access());
	new_thread->setup((void *)&thorRtThreadEntry, (uintptr_t)entry);
	new_thread->setProcess(cur_process);
	new_thread->setAddressSpace(address_space);

	auto descriptor = new (memory::kernelAllocator.access()) Thread::ThreadDescriptor(new_thread->unsafe<Thread>());
	cur_process->attachDescriptor(descriptor);
	return descriptor->getHandle();
}

void helSwitchThread(HelDescriptor thread_handle) {
	UnsafePtr<Thread> cur_thread = (*currentThread)->unsafe<Thread>();
	UnsafePtr<Process> cur_process = cur_thread->getProcess();
	
	auto thread_descriptor = (Thread::ThreadDescriptor *)cur_process->getDescriptor(thread_handle);
	UnsafePtr<Thread> thread = thread_descriptor->getThread();

	thread->switchTo();
}

