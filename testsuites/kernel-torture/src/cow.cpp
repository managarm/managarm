#include <hel.h>
#include <hel-syscalls.h>

#include "testsuite.hpp"

DEFINE_TEST(cows, ([] {
	HelHandle handle;
	HEL_CHECK(helCopyOnWrite(kHelZeroMemory, 0, 0x1000, &handle));

	void *window;
	HEL_CHECK(helMapMemory(handle, kHelNullHandle, nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite, &window));
	*reinterpret_cast<volatile uintptr_t *>(window) = 0xDEADBEEF;
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, 0x1000));

	HelHandle forkHandle;
	HEL_CHECK(helForkMemory(handle, &forkHandle));
	HEL_CHECK(helMapMemory(forkHandle, kHelNullHandle, nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite, &window));
	*reinterpret_cast<volatile uintptr_t *>(window) = 0xC0FFEE;
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, 0x1000));

	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, handle));
	HEL_CHECK(helCloseDescriptor(kHelThisUniverse, forkHandle));
}))
