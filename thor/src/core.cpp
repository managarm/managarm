
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

namespace thor {

LazyInitializer<util::Vector<Resource *, KernelAllocator>> resourceMap;

void Resource::install() {
	p_resHandle = resourceMap->size();
	resourceMap->push(this);
}

Handle Resource::getResHandle() {
	return p_resHandle;
}

// --------------------------------------------------------
// MemoryResource
// --------------------------------------------------------

MemoryResource::MemoryResource()
		: p_physicalPages(memory::kernelAllocator.access()) { }

void MemoryResource::resize(size_t length) {
	for(size_t l = 0; l < length; l += 0x1000)
		p_physicalPages.push(memory::tableAllocator->allocate());
}

} // namespace thor

