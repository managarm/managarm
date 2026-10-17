#pragma once

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

} // namespace thor
