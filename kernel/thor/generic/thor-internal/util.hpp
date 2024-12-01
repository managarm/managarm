#pragma once

#include <assert.h>
#include <stdint.h>

namespace thor {

int ceil_log2(unsigned long x) { return 8 * sizeof(unsigned long) - __builtin_clzl(x); }

// Helper class to store the frequency or inverse frequency (= tick duration) of a timer.
// Designed to support the conversion of ticks into durations and vice versa with high accuracy.
// The fraction is represented as (f / 2^s) where f is a 64-bit factor and s is a scaling exponent.
// When doing conversions, the multiplication is done in 128-bit to avoid loss of precision.
struct FreqFraction {
	uint64_t operator*(uint64_t rhs) {
		auto product = (static_cast<__uint128_t>(f) * static_cast<__uint128_t>(rhs)) >> s;
		assert(!(product >> 64));
		return static_cast<uint64_t>(product);
	}

	uint64_t f{0};
	int s{0};
};

// Converts the fraction (num / denom) to a FreqFraction.
FreqFraction computeFreqFraction(uint64_t num, uint64_t denom) {
	// TODO: We could use a higher shift (i.e., subtract floor_log2(denom))
	//       since the division by denom would bring the number back below 64-bit.
	//       For now, we do not use this fact as it requires a 128-bit division.
	auto s = 63 - ceil_log2(num);
	auto f = (num << s) / denom;
	return FreqFraction{f, s};
}

} // namespace thor
