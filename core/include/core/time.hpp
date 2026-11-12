#pragma once

#include <algorithm>
#include <stdint.h>
#include <sys/time.h>

namespace core {

// Saturates at UINT64_MAX. Negative timevals give 0 (with SO_RCVTIMEO, they time out at once).
constexpr uint64_t timevalToNanos(const timeval &tv) {
	if(tv.tv_sec < 0)
		return 0;
	uint64_t ns;
	if(__builtin_mul_overflow(uint64_t(tv.tv_sec), uint64_t{1'000'000'000}, &ns))
		return UINT64_MAX;
	auto usec = std::clamp<int64_t>(tv.tv_usec, 0, 999'999);
	if(__builtin_add_overflow(ns, uint64_t(usec) * 1'000, &ns))
		return UINT64_MAX;
	return ns;
}

} // namespace core
