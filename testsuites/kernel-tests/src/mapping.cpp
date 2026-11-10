#include <cassert>
#include <cstddef>
#include <iostream>

#include <core/process-data.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <helix/memory.hpp>

#include "testsuite.hpp"

DEFINE_TEST(unmapUnalignedMapping, ([] {
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), 2 * kHelPageSize,
			0, nullptr, &handle));
	helix::UniqueDescriptor memory{handle};
	void *base;
	{
		// The logical length is one page, but the offset makes the mapping
		// occupy two pages. Destruction must release both of them.
		helix::Mapping mapping{memory, 1, kHelPageSize};
		auto bytes = static_cast<volatile char *>(mapping.get());
		bytes[0] = 1;
		bytes[kHelPageSize - 1] = 2;
		base = static_cast<char *>(mapping.get()) - 1;
	}

	void *replacement;
	HEL_CHECK(helMapMemory(handle, kHelNullHandle, base, 0, 2 * kHelPageSize,
			kHelMapFixedNoReplace | kHelMapProtRead | kHelMapProtWrite, &replacement));
	assert(replacement == base);
	HEL_CHECK(helUnmapMemory(kHelNullHandle, replacement, 2 * kHelPageSize));
}))

DEFINE_TEST(unmapPartialPfs, ([] {
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), (3 * kHelPageSize), 0, nullptr, &handle));
	void *window;
	HEL_CHECK(helMapMemory(handle, kHelNullHandle, nullptr, 0, (3 * kHelPageSize),
			kHelMapProtRead | kHelMapProtWrite, &window));

	// Do the partial unmap.
	auto p = reinterpret_cast<std::byte *>(window);
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p + kHelPageSize, kHelPageSize));

	// Touch the page to make sure that the new mappings work.
	p[0] = static_cast<std::byte>(0);
	p[(2 * kHelPageSize)] = static_cast<std::byte>(0);

	// Clean up.
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p, kHelPageSize));
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p + (2 * kHelPageSize), kHelPageSize));
}))

DEFINE_TEST(unmapPartialPreserve, ([] {
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), (3 * kHelPageSize), 0, nullptr, &handle));
	void *window;
	HEL_CHECK(helMapMemory(handle, kHelNullHandle, nullptr, 0, (3 * kHelPageSize),
			kHelMapProtRead | kHelMapProtWrite, &window));

	auto p = reinterpret_cast<std::byte *>(window);
	p[0] = static_cast<std::byte>(42);
	p[(2 * kHelPageSize)] = static_cast<std::byte>(21);

	// Do the partial unmap.
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p + kHelPageSize, kHelPageSize));

	// Check that the values are preserved after partially unmapping.
	assert(p[0] == static_cast<std::byte>(42));
	assert(p[(2 * kHelPageSize)] == static_cast<std::byte>(21));

	// Clean up.
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p, kHelPageSize));
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p + (2 * kHelPageSize), kHelPageSize));
}))
