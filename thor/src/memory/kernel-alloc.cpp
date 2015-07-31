
#include "../kernel.hpp"

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
// StupidMemoryAllocator::Header
// --------------------------------------------------------

StupidMemoryAllocator::Header::Header(size_t num_pages)
: numPages(num_pages) { }

// --------------------------------------------------------
// StupidMemoryAllocator
// --------------------------------------------------------

void *StupidMemoryAllocator::allocate(size_t length) {
	size_t with_header = length + sizeof(Header);

	size_t num_pages = with_header / kPageSize;
	if((with_header % kPageSize) != 0)
		num_pages++;

	void *pointer = p_virtualAllocator.allocate(with_header);
	for(size_t offset = 0; offset < with_header; offset += kPageSize) {
		PhysicalAddr physical = tableAllocator->allocate(1);
		kernelSpace->mapSingle4k((char *)pointer + offset, physical);
	}
	thorRtInvalidateSpace();
	asm("" : : : "memory");

	Header *header = (Header *)pointer;
	new (header) Header(num_pages);
	
	return (void *)((uintptr_t)pointer + sizeof(Header));
}

void StupidMemoryAllocator::free(void *pointer) {
	if(pointer == nullptr)
		return;
	
	Header *header = (Header *)((uintptr_t)pointer - sizeof(Header));
	
	size_t num_pages = header->numPages;

	asm("" : : : "memory");
	for(size_t i = 0; i < num_pages; i++) {
		VirtualAddr virt = (VirtualAddr)header + i * kPageSize;
		PhysicalAddr physical = kernelSpace->unmapSingle4k(virt);
		tableAllocator->free(physical);
	}
	thorRtInvalidateSpace();
}

}} // namespace thor::memory

