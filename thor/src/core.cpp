
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

LazyInitializer<SharedPtr<Thread>> currentThread;

// --------------------------------------------------------
// Descriptor
// --------------------------------------------------------

Descriptor::Descriptor()
		: p_handle(0) { }

Handle Descriptor::getHandle() {
	return p_handle;
}

// --------------------------------------------------------
// Process
// --------------------------------------------------------

Process::Process()
		: p_descriptorMap(memory::kernelAllocator.access()) { }

void Process::attachDescriptor(Descriptor *descriptor) {
	descriptor->p_handle = p_descriptorMap.size();
	p_descriptorMap.push(descriptor);
}

Descriptor *Process::getDescriptor(Handle handle) {
	return p_descriptorMap[handle];
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

AddressSpace::AddressSpace(memory::PageSpace page_space)
		: p_pageSpace(page_space) { }

void AddressSpace::mapSingle4k(void *address, uintptr_t physical) {
	p_pageSpace.mapSingle4k(address, physical);
}

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

void Thread::setup(void *entry, uintptr_t argument) {
	size_t stack_size = 0x2000;

	char *stack_base = (char *)memory::kernelAllocator->allocate(stack_size);
	uint64_t *stack_ptr = (uint64_t *)(stack_base + stack_size);
	stack_ptr--; *stack_ptr = (uint64_t)entry;
	
	p_state.rbx = (uintptr_t)argument;
	p_state.rsp = stack_ptr;
}

UnsafePtr<Process> Thread::getProcess() {
	return p_process->unsafe<Process>();
}
UnsafePtr<AddressSpace> Thread::getAddressSpace() {
	return p_addressSpace->unsafe<AddressSpace>();
}

void Thread::setProcess(UnsafePtr<Process> process) {
	p_process = process->shared<Process>();
}
void Thread::setAddressSpace(UnsafePtr<AddressSpace> address_space) {
	p_addressSpace = address_space->shared<AddressSpace>();
}

void Thread::switchTo() {
	SharedPtr<Thread> cur_thread_res = *currentThread;
	*currentThread = this->shared<Thread>();

	thorRtSwitchThread(&cur_thread_res->p_state, &this->p_state);
}

// --------------------------------------------------------
// Thread::ThreadDescriptor
// --------------------------------------------------------

Thread::ThreadDescriptor::ThreadDescriptor(UnsafePtr<Thread> thread)
		: p_thread(thread->shared<Thread>()) { }

UnsafePtr<Thread> Thread::ThreadDescriptor::getThread() {
	return p_thread->unsafe<Thread>();
}

// --------------------------------------------------------
// Memory
// --------------------------------------------------------

Memory::Memory()
		: p_physicalPages(memory::kernelAllocator.access()) { }

void Memory::resize(size_t length) {
	for(size_t l = 0; l < length; l += 0x1000)
		p_physicalPages.push(memory::tableAllocator->allocate());
}

uintptr_t Memory::getPage(int index) {
	return p_physicalPages[index];
}

// --------------------------------------------------------
// Memory::AccessDescriptor
// --------------------------------------------------------

Memory::AccessDescriptor::AccessDescriptor(UnsafePtr<Memory> memory)
		: p_memory(memory->shared<Memory>()) { }

UnsafePtr<Memory> Memory::AccessDescriptor::getMemory() {
	return p_memory->unsafe<Memory>();
}

} // namespace thor

