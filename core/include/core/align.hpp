#pragma once

#include <assert.h>
#include <tuple>
#include <stddef.h>
#include <stdint.h>

namespace core {

// return an alignment-aligned superset of the span
inline std::tuple<uintptr_t, size_t> alignExtend(std::tuple<uintptr_t, size_t> span, size_t alignment) {
	assert((alignment & (alignment - 1)) == 0);

	auto [addr, size] = span;
	// align address down to the alignment
	auto alignedAddr = addr & ~(alignment - 1);
	auto alignedSize = (size + (addr - alignedAddr) + (alignment - 1)) & ~(alignment - 1);
	return {alignedAddr, alignedSize};
}

} // namespace core
