
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

namespace thor {

LazyInitializer<util::Vector<Resource *, KernelAllocator>> resourceMap;

LazyInitializer<RefCountPtr<ThreadResource>> currentThread;

void Resource::install() {
	p_resHandle = resourceMap->size();
	resourceMap->push(this);
}

Handle Resource::getResHandle() {
	return p_resHandle;
}

// --------------------------------------------------------
// AddressSpaceResource
// --------------------------------------------------------

AddressSpaceResource::AddressSpaceResource(memory::PageSpace page_space)
		: p_pageSpace(page_space) { }

void AddressSpaceResource::mapSingle4k(void *address, uintptr_t physical) {
	p_pageSpace.mapSingle4k(address, physical);
}

// --------------------------------------------------------
// ThreadResource
// --------------------------------------------------------

RefCountPtr<AddressSpaceResource> ThreadResource::getAddressSpace() {
	return p_addressSpace;
}

void ThreadResource::setAddressSpace(RefCountPtr<AddressSpaceResource> address_space) {
	p_addressSpace = address_space;
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

uintptr_t MemoryResource::getPage(int index) {
	return p_physicalPages[index];
}

} // namespace thor

