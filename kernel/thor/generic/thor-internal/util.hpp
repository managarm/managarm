#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <bit>
#include <span>
#include <type_traits>

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
