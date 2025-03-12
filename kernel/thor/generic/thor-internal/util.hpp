#pragma once

#include <assert.h>
#include <stdint.h>

namespace thor {

inline int ceil_log2(unsigned long x) { return 8 * sizeof(unsigned long) - __builtin_clzl(x); }

// Helper class to store the frequency or inverse frequency (= tick duration) of a timer.
// Designed to support the conversion of ticks into durations and vice versa with high accuracy.
// The fraction is represented as (f / 2^s) where f is a 64-bit factor and s is a scaling exponent.
// When doing conversions, the multiplication is done in 128-bit to avoid loss of precision.
struct FreqFraction {
	explicit operator bool () {
		return f;
	}

	// Saturating multiplication.
	// If the fraction is > 1, the result may be clamped to UINT64_MAX for large RHS.
	// When implementing timers using this function, callers should always check whether the
	// timer as truly expired or not (and re-arm the timer as necessary).
	// Clamping is usually not an issue when converting ticks (since boot) to nanoseconds
	// as the system will not be up for 2^64 nanoseconds.
	uint64_t operator*(uint64_t rhs) {
		auto product = (static_cast<__uint128_t>(f) * static_cast<__uint128_t>(rhs)) >> s;
		if (product >> 64)
			return UINT64_MAX;
		return static_cast<uint64_t>(product);
	}

	uint64_t f{0};
	int s{0};
};

// Converts the fraction (num / denom) to a FreqFraction.
inline FreqFraction computeFreqFraction(uint64_t num, uint64_t denom) {
	// TODO: We could use a higher shift (i.e., subtract floor_log2(denom))
	//       since the division by denom would bring the number back below 64-bit.
	//       For now, we do not use this fact as it requires a 128-bit division.
	auto s = 63 - ceil_log2(num);
	auto f = (num << s) / denom;
	return FreqFraction{f, s};
}

} // namespace thor
