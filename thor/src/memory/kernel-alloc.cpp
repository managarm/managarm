
#include "../../../frigg/include/arch_x86/types64.hpp"
#include "../runtime.hpp"
#include "physical-alloc.hpp"
#include "paging.hpp"
#include "kernel-alloc.hpp"

namespace thor {
namespace memory {

LazyInitializer<StupidMemoryAllocator> kernelAllocator;

// --------------------------------------------------------
// StupidVirtualAllocator
// --------------------------------------------------------

StupidVirtualAllocator::StupidVirtualAllocator() : p_nextPointer((char *)0x800000) { }

void *StupidVirtualAllocator::allocate(size_t length) {
	if(length % kPageSize != 0)
		length += kPageSize - (length % kPageSize);

	char *pointer = p_nextPointer;
	p_nextPointer += length;
	return pointer;
}

// --------------------------------------------------------
// StupidMemoryAllocator
// --------------------------------------------------------

void *StupidMemoryAllocator::allocate(size_t length) {
	void *pointer = p_virtualAllocator.allocate(length);
	for(size_t offset = 0; offset < length; offset += kPageSize)
		kernelSpace->mapSingle4k((char *)pointer + offset, tableAllocator->allocate());
	thorRtInvalidateSpace();
	return pointer;
}

}} // namespace thor::memory

void *operator new(size_t length, thor::memory::StupidMemoryAllocator *allocator) {
	return allocator->allocate(length);
}

