
#include "../../frigg/include/arch_x86/types64.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "../../frigg/include/arch_x86/gdt.hpp"
#include "../../frigg/include/arch_x86/idt.hpp"
#include "../../frigg/include/elf.hpp"
#include "util/vector.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"
#include "core.hpp"
#include "../../hel/include/hel.h"

using namespace thor;

hel_resource_t hel_create_memory(size_t length) {
	MemoryResource *resource = new (memory::kernelAllocator.access()) MemoryResource();
	resource->install();
	resource->resize(length);
	return resource->getResHandle();
}

