
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

HelResource helCreateMemory(size_t length) {
	auto resource = makeShared<MemoryResource>(memory::kernelAllocator.access());
	resource->install();
	resource->resize(length);
	return resource->getResHandle();
}

void helMapMemory(HelResource resource, void *pointer, size_t length) {
	SharedPtr<ThreadResource> thread = *currentThread.access();
	SharedPtr<AddressSpaceResource> address_space = thread->getAddressSpace();
	UnsafePtr<MemoryResource> memory = (*resourceMap.access())[(int)resource]->unsafe<MemoryResource>();

	for(int offset = 0, i = 0; offset < length; offset += 0x1000, i++) {
		address_space->mapSingle4k((void *)((uintptr_t)pointer + offset),
				memory->getPage(i));
	}
	
	thorRtInvalidateSpace();
}

HelResource helCreateThread(void *entry) {
	SharedPtr<ThreadResource> thread = *currentThread.access();
	SharedPtr<AddressSpaceResource> address_space = thread->getAddressSpace();

	auto resource = makeShared<ThreadResource>(memory::kernelAllocator.access());
	resource->install();
	resource->setup((void *)&thorRtThreadEntry, (uintptr_t)entry);
	resource->setAddressSpace(address_space);
	return resource->getResHandle();
}

void helSwitchThread(HelResource thread_handle) {
	SharedPtr<ThreadResource> cur_thread = *currentThread.access();
	UnsafePtr<ThreadResource> next_thread = (*resourceMap.access())[(int)thread_handle]->unsafe<ThreadResource>();
	next_thread->switchTo();
}

