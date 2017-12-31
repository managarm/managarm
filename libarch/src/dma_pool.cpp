
#include <iostream>
#include <arch/dma_pool.hpp>
#include <hel.h>
#include <hel-syscalls.h>

namespace arch {
namespace os {

uintptr_t contiguous_policy::map(size_t length) {
	assert((length % 0x1000) == 0);

	HelHandle memory;
	void *actual_ptr;
	HEL_CHECK(helAllocateMemory(length, kHelAllocContinuous, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, 0, length,
			kHelMapProtRead | kHelMapProtWrite | kHelMapCopyOnWriteAtFork, &actual_ptr));
	HEL_CHECK(helCloseDescriptor(memory));
	return (uintptr_t)actual_ptr;
}

void contiguous_policy::unmap(uintptr_t address, size_t length) {
	HEL_CHECK(helUnmapMemory(kHelNullHandle, (void *)address, length));
}

contiguous_pool::contiguous_pool()
: _allocator(_policy) { }

void *contiguous_pool::allocate(size_t size, size_t count, size_t align) {
	// We do not have to pay attention to the alignment parameter
	// because the frigg slab allocator always returns naturally aligned chunks.
	(void)align;
	// TODO: Check for overflow.
	return _allocator.allocate(size * count);
}

void contiguous_pool::deallocate(void *pointer, size_t size, size_t count, size_t align) {
	(void)size;
	(void)count;
	(void)align;
	_allocator.free(pointer);
}

} } // namespace arch::os

