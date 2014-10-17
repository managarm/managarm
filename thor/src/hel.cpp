
#include "../../frigg/include/arch_x86/types64.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "../../frigg/include/arch_x86/gdt.hpp"
#include "../../frigg/include/arch_x86/idt.hpp"
#include "../../frigg/include/elf.hpp"
#include "util/vector.hpp"
#include "util/smart-ptr.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"
#include "core.hpp"
#include "../../hel/include/hel.h"

using namespace thor;

HelResource helCreateMemory(size_t length) {
	auto resource = RefCountPtr<MemoryResource>::make(memory::kernelAllocator.access());
	resource->install();
	resource->resize(length);
	return resource->getResHandle();
}

void helMapMemory(HelResource resource, void *pointer, size_t length) {
	RefCountPtr<ThreadResource> thread = *currentThread.access();
	RefCountPtr<AddressSpaceResource> address_space = thread->getAddressSpace();
	MemoryResource *memory = (MemoryResource *)(*resourceMap.access())[(int)resource];

	for(int offset = 0, i = 0; offset < length; offset += 0x1000, i++) {
		address_space->mapSingle4k((void *)((uintptr_t)pointer + offset),
				memory->getPage(i));
	}
	
	thorRtInvalidateSpace();
}

