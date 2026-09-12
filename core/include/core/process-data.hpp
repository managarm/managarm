#pragma once

#include <hel.h>

namespace core {

// Returns a cached handle to the calling process's own hierarchy capability.
// Note: the handle becomes invalid after fork() and there is currently no way to re-cache it.
HelHandle getProcessHierarchy();

} // namespace core
