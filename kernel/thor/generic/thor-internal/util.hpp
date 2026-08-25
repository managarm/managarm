#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <bit>
#include <span>
#include <type_traits>

namespace thor {

inline int ceil_log2(unsigned long x) { return 8 * sizeof(unsigned long) - __builtin_clzl(x); }

// Direction in which a Pow2Fraction rounds the value that it represents.
enum class Rounding { down, up };

// Helper class to store a fraction as (f / 2^s) where f is a 64-bit factor and s is a scaling
// exponent. Used for the frequency and inverse frequency (= tick duration) of timers, i.e. to
// convert ticks into durations and vice versa with high accuracy.
// When doing conversions, the multiplication is done in 128-bit to avoid loss of precision.
// Both the representation and the multiplication round towards R,
// i.e. conversions yield an lower/upper bound on the true value.
template<Rounding R>
struct Pow2Fraction {
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
		auto product = static_cast<__uint128_t>(f) * static_cast<__uint128_t>(rhs);
		auto quotient = product >> s;
		if constexpr (R == Rounding::up) {
			if (product & ((static_cast<__uint128_t>(1) << s) - 1))
				++quotient;
		}
		if (quotient >> 64)
			return UINT64_MAX;
		return static_cast<uint64_t>(quotient);
	}

	uint64_t f{0};
	int s{0};
};

// Converts the fraction (num / denom) to a Pow2Fraction, rounding the representation towards R.
template<Rounding R>
inline Pow2Fraction<R> computePow2Fraction(uint64_t num, uint64_t denom) {
	// TODO: We could use a higher shift (i.e., subtract floor_log2(denom))
	//       since the division by denom would bring the number back below 64-bit.
	//       For now, we do not use this fact as it requires a 128-bit division.
	auto s = 63 - ceil_log2(num);
	auto scaled = num << s;
	auto f = scaled / denom;
	if constexpr (R == Rounding::up) {
		if (scaled % denom)
			++f;
	}
	return Pow2Fraction<R>{f, s};
}

// Computes ceil(2^exp / divisor) using only 64-bit divisions, i.e. by long division.
// The caller picks exp such that the quotient fits into 64 bits.
inline uint64_t ceilPow2Divide(int exp, uint64_t divisor) {
	assert(divisor > 1);

	// Consume the leading one of the dividend, then shift in its zeros one at a time.
	uint64_t quotient = 0;
	uint64_t remainder = 1;
	for (int i = 0; i < exp; ++i) {
		// The doubled remainder does not fit into 64 bits iff it exceeds the divisor anyway.
		bool overflow = remainder >> 63;
		remainder <<= 1;
		quotient <<= 1;
		if (overflow || remainder >= divisor) {
			remainder -= divisor;
			quotient |= 1;
		}
	}
	return remainder ? quotient + 1 : quotient;
}

// Computes the reciprocal of the given fraction such that
// (fraction * (computeReciprocal(fraction) * value)) >= value for all values.
// Timers need this direction: converting a deadline to ticks and back must not land before the
// deadline, or the timer fires before the deadline is considered to be reached.
inline Pow2Fraction<Rounding::up> computeReciprocal(Pow2Fraction<Rounding::down> fraction) {
	// The reciprocal is 2^fraction.s / fraction.f. Scale it such that it keeps 63 bits.
	auto exp = 62 + ceil_log2(fraction.f);
	return Pow2Fraction<Rounding::up>{ceilPow2Divide(exp, fraction.f), exp - fraction.s};
}

template <size_t Mod = std::dynamic_extent>
struct QueueIndex {
private:
	struct StaticStorage {
		constexpr StaticStorage() noexcept = default;
		constexpr size_t get() const noexcept { return Mod; }
	};

	struct DynamicStorage {
		constexpr explicit DynamicStorage(size_t mod) noexcept : mod_(mod) {}
		constexpr size_t get() const noexcept { return mod_; }

	private:
		size_t mod_;
	};

	using StorageType =
	    std::conditional_t<Mod == std::dynamic_extent, DynamicStorage, StaticStorage>;

public:
	constexpr explicit QueueIndex(size_t value) noexcept
	    requires(Mod != std::dynamic_extent)
	: index_(value % Mod),
	  modStorage_() {
		[[assume(Mod > 0)]];
		[[assume(index_ < Mod)]];
	}

	constexpr QueueIndex(size_t value, size_t mod) noexcept
	    requires(Mod == std::dynamic_extent)
	: index_(value % mod),
	  modStorage_(mod) {
		[[assume(mod > 0)]];
		[[assume(index_ < mod)]];
	}

	constexpr operator size_t() const noexcept { return index_; }

	constexpr size_t operator()() const noexcept { return index_; }

	constexpr QueueIndex operator+(int v) const noexcept {
		size_t current_mod = modStorage_.get();
		[[assume(current_mod > 0)]];

		QueueIndex tmp = *this;

		if constexpr (Mod != std::dynamic_extent && std::has_single_bit(Mod)) {
			tmp.index_ = (tmp.index_ + static_cast<size_t>(v)) & (Mod - 1uz);
		} else {
			if (v >= 0) {
				tmp.index_ = (tmp.index_ + (static_cast<size_t>(v) % current_mod)) % current_mod;
			} else {
				size_t abs_v = static_cast<size_t>(-v) % current_mod;
				tmp.index_ = (current_mod + tmp.index_ - abs_v) % current_mod;
			}
		}
		return tmp;
	}

	constexpr QueueIndex &operator++() noexcept {
		size_t current_mod = modStorage_.get();
		[[assume(current_mod > 0)]];

		if constexpr (Mod != std::dynamic_extent && std::has_single_bit(Mod)) {
			index_ = (index_ + 1uz) & (Mod - 1uz);
		} else {
			index_ = (index_ + 1uz) % current_mod;
		}

		return *this;
	}

	constexpr QueueIndex operator++(int) noexcept {
		auto temp = *this;
		++*this;
		return temp;
	}

	constexpr QueueIndex &operator--() noexcept {
		size_t current_mod = modStorage_.get();
		[[assume(current_mod > 0)]];

		if constexpr (Mod != std::dynamic_extent && std::has_single_bit(Mod)) {
			index_ = (index_ - 1uz) & (Mod - 1uz);
		} else {
			index_ = (current_mod + index_ - 1uz) % current_mod;
		}

		return *this;
	}

	constexpr QueueIndex operator--(int) noexcept {
		auto temp = *this;
		--(*this);
		return temp;
	}

	constexpr bool operator==(const QueueIndex &other) const noexcept {
		return modStorage_.get() == other.modStorage_.get() && index_ == other.index_;
	}

private:
	size_t index_;
	[[no_unique_address]] StorageType modStorage_;
};

} // namespace thor
