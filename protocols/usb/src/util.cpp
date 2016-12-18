
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include <deque>
#include <experimental/optional>

#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <helix/await.hpp>
#include <mbus.hpp>
#include <protocols/usb/client.hpp>

uintptr_t ContiguousPolicy::map(size_t length) {
	assert((length % 0x1000) == 0);

	HelHandle memory;
	void *actual_ptr;
	HEL_CHECK(helAllocateMemory(length, kHelAllocContinuous, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, 0, length,
			kHelMapReadWrite | kHelMapCopyOnWriteAtFork, &actual_ptr));
	HEL_CHECK(helCloseDescriptor(memory));
	return (uintptr_t)actual_ptr;
}

void ContiguousPolicy::unmap(uintptr_t address, size_t length) {
	HEL_CHECK(helUnmapMemory(kHelNullHandle, (void *)address, length));
}

ContiguousPolicy contiguousPolicy;
ContiguousAllocator contiguousAllocator(contiguousPolicy);

