
#include "../../../frigg/include/types.hpp"
#include "../util/general.hpp"
#include "../runtime.hpp"
#include "physical-alloc.hpp"
#include "paging.hpp"
#include "kernel-alloc.hpp"

namespace thor {
namespace memory {

// --------------------------------------------------------
// StupidVirtualAllocator
// --------------------------------------------------------

StupidVirtualAllocator::StupidVirtualAllocator() : p_nextPointer((char *)0xFFFF800200000000) { }

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
		kernelSpace->mapSingle4k((char *)pointer + offset, tableAllocator->allocate(1));
	thorRtInvalidateSpace();
	return pointer;
}

}} // namespace thor::memory

