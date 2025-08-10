#include <arch/dma_pool.hpp>
#include <hel-syscalls.h>
#include <hel.h>

namespace arch {

uintptr_t contiguous_policy::map(size_t length) {
	assert((length % 0x1000) == 0);

	HelAllocRestrictions restrictions;
	restrictions.addressBits = _options.addressBits;

	HelHandle memory;
	void *actual_ptr;
	HEL_CHECK(helAllocateMemory(length, kHelAllocContinuous, &restrictions, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, 0, length,
			kHelMapProtRead | kHelMapProtWrite, &actual_ptr));
	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, memory));
	return (uintptr_t)actual_ptr;
}

void contiguous_policy::unmap(uintptr_t address, size_t length) {
	HEL_CHECK(helUnmapMemory(kHelNullHandle, (void *)address, length));
}

contiguous_pool::contiguous_pool(contiguous_pool_options options)
: _policy{options}, _slab{_policy} { }

void *contiguous_pool::allocate(size_t size, size_t count, size_t align) {
	// We do not have to pay attention to the alignment parameter
	// because the frigg slab allocator always returns naturally aligned chunks.
	(void)align;
	// TODO: Check for overflow.
	return _slab.allocate(size * count);
}

void contiguous_pool::deallocate(void *pointer, size_t size, size_t count, size_t align) {
	(void)size;
	(void)count;
	(void)align;
	_slab.free(pointer);
}

} // namespace arch
