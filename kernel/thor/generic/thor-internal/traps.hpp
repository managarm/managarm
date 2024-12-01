#pragma once

#include <thor-internal/arch-generic/cpu.hpp>

namespace thor {

// Determine whether the fault is an UAR fault, and handle it appropriately if so.
bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor);

} // namespace thor
