#pragma once

#include <async/result.hpp>
#include <stddef.h>

namespace core {

struct MemoryInformation {
	// Both values are in bytes.
	size_t totalMemory;
	size_t availableMemory;
};

// Queries kerncfg, connecting to it on first use.
async::result<MemoryInformation> getMemoryInformation();

} // namespace core
