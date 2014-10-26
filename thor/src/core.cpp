
#include "../../frigg/include/arch_x86/types64.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "util/vector.hpp"
#include "util/smart-ptr.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"
#include "core.hpp"

namespace thor {

LazyInitializer<util::Vector<UnsafePtr<Resource>, KernelAllocator>> resourceMap;

LazyInitializer<SharedPtr<ThreadResource>> currentThread;

Handle Resource::getResHandle() {
	return p_resHandle;
}

void Resource::install() {
	p_resHandle = resourceMap->size();
	resourceMap->push(this->unsafe<Resource>());
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

void ThreadResource::setup(void *entry, uintptr_t argument) {
	size_t stack_size = 0x2000;

	char *stack_base = (char *)memory::kernelAllocator->allocate(stack_size);
	uint64_t *stack_ptr = (uint64_t *)(stack_base + stack_size);
	stack_ptr--; *stack_ptr = (uint64_t)entry;
	
	p_state.rbx = (uintptr_t)argument;
	p_state.rsp = stack_ptr;
}

SharedPtr<AddressSpaceResource> ThreadResource::getAddressSpace() {
	return p_addressSpace;
}

void ThreadResource::setAddressSpace(SharedPtr<AddressSpaceResource> address_space) {
	p_addressSpace = address_space;
}

void ThreadResource::switchTo() {
	SharedPtr<ThreadResource> cur_thread_res = *currentThread;
	*currentThread = this->shared<ThreadResource>();

	thorRtSwitchThread(&cur_thread_res->p_state, &this->p_state);
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

