#pragma once

#include <stddef.h>

namespace thor {

// Determines the floor below which a physical allocation is refused.
// Earlier classes take precedence.
enum class AllocationClass {
	// Allocations that cannot fail. Has no floor.
	emergency,
	// Kernel metadata. Refused below the kernel floor.
	kernel,
	// Frames that back user memory. Refused below the user floor.
	user,
};

struct AllocationPolicy {
	AllocationClass cls;
	// Pinnable pages kept free on top of the floor of cls,
	// so that allocations with lower floors can still proceed.
	size_t floor;
};

} // namespace thor
