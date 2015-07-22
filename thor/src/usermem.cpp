
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

namespace thor {

// --------------------------------------------------------
// Memory
// --------------------------------------------------------

Memory::Memory()
		: p_physicalPages(kernelAlloc.get()) { }

void Memory::resize(size_t length) {
	for(size_t l = 0; l < length; l += 0x1000) {
		uintptr_t page = memory::tableAllocator->allocate(1);
		p_physicalPages.push(page);
	}
}

uintptr_t Memory::getPage(int index) {
	return p_physicalPages[index];
}

// --------------------------------------------------------
// MemoryAccessDescriptor
// --------------------------------------------------------

MemoryAccessDescriptor::MemoryAccessDescriptor(SharedPtr<Memory> &&memory)
		: p_memory(util::move(memory)) { }

UnsafePtr<Memory> MemoryAccessDescriptor::getMemory() {
	return p_memory->unsafe<Memory>();
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

AddressSpace::AddressSpace(memory::PageSpace page_space)
		: p_pageSpace(page_space) { }

void AddressSpace::mapSingle4k(void *address, uintptr_t physical) {
	p_pageSpace.mapSingle4k(address, physical);
}

} // namespace thor

