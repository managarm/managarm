#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <bit>

#include <linux/types.h>
#include <sound/asound.h>

namespace alsa {

struct Params {
	constexpr snd_mask &mask(uint32_t what) {
		return values.masks[what - SNDRV_PCM_HW_PARAM_FIRST_MASK];
	}

	constexpr snd_interval &interval(uint32_t what) {
		return values.intervals[what - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
	}

	constexpr void intersect_interval(uint32_t what, snd_interval other) {
		auto &range = interval(what);

		bool changed = false;

		if (other.min > range.min) {
			range.min = other.min;
			range.openmin = other.openmin;
			changed = true;
		} else if (other.min == range.min) {
			changed |= other.openmin && !range.openmin;
			range.openmin |= other.openmin;
		}

		if (other.max < range.max) {
			range.max = other.max;
			range.openmax = other.openmax;
			changed = true;
		} else if (other.max == range.max) {
			changed |= other.openmax && !range.openmax;
			range.openmax |= other.openmax;
		}

		changed |= other.integer && !range.integer;
		range.integer |= other.integer;

		if (range.integer) {
			if (range.openmin) {
				range.min++;
				range.openmin = 0;
			}

			if (range.openmax) {
				range.max--;
				range.openmax = 0;
			}
		} else if (!range.openmin && !range.openmax && range.min == range.max) {
			range.integer = 1;
		}

		if (range.min > range.max ||
				(range.min == range.max && (range.openmin || range.openmax))) {
			changed |= !range.empty;
			range.empty = 1;
		} else {
			changed |= range.empty;
			range.empty = 0;
		}

		if (changed)
			values.cmask |= 1U << what;
	}

	snd_pcm_hw_params values;
};

struct Rule {
	template<size_t N>
	Rule(uint32_t what, const uint32_t (&deps)[N], std::function<void (Params &params)> func)
			: what{what}, func{std::move(func)} {
		for (auto dep : deps) {
			dependencies |= 1U << dep;
		}
	}

	uint32_t what;
	uint32_t dependencies{};
	std::function<void (Params &params)> func;
};

namespace utils {

constexpr uint32_t safeDiv32(uint32_t value, uint32_t divisor, uint32_t& remainder) {
	if (divisor == 0) {
		remainder = 0;
		return UINT32_MAX;
	}

	remainder = value % divisor;
	return value / divisor;
}

constexpr uint32_t safeMul32(uint32_t value, uint32_t multiplier) {
	if (value == 0)
		return value;

	if (UINT32_MAX / value < multiplier)
		return UINT32_MAX;

	return value * multiplier;
}

constexpr uint32_t safeMuldiv32(uint32_t value, uint32_t multiplier, uint32_t divisor, uint32_t &remainder) {
	if (divisor == 0) {
		remainder = 0;
		return UINT32_MAX;
	}

	auto result = static_cast<uint64_t>(value) * static_cast<uint64_t>(multiplier);

	remainder = result % divisor;
	result /= divisor;

	if (result > UINT32_MAX) {
		remainder = 0;
		return UINT32_MAX;
	}

	return result;
}

constexpr snd_interval intervalMul(snd_interval value, snd_interval multiplier) {
	snd_interval result{};

	if (value.empty || multiplier.empty) {
		result.empty = 1;
		return result;
	}

	result.min = safeMul32(value.min, multiplier.min);
	result.openmin = value.openmin || multiplier.openmin;
	result.max = safeMul32(value.max, multiplier.max);
	result.openmax = value.openmax || multiplier.openmax;
	result.integer = value.integer && multiplier.integer;

	return result;
}

constexpr snd_interval intervalDiv(snd_interval value, snd_interval divisor) {
	snd_interval result{};

	if (value.empty || divisor.empty) {
		result.empty = 1;
		return result;
	}

	uint32_t remainder;
	result.min = safeDiv32(value.min, divisor.max, remainder);
	result.openmin = remainder != 0 || value.openmin || divisor.openmax;
	if (divisor.min > 0) {
		result.max = safeDiv32(value.max, divisor.min, remainder);
		if (remainder != 0) {
			result.max++;
			result.openmax = 1;
		} else {
			result.openmax = value.openmax || divisor.openmin;
		}
	} else {
		result.max = UINT32_MAX;
		result.openmax = 0;
	}

	return result;
}

constexpr snd_interval intervalMulDiv(const snd_interval &value, uint32_t multiplier, const snd_interval &divisor) {
	snd_interval result{};

	if (value.empty || divisor.empty) {
		result.empty = 1;
		return result;
	}

	uint32_t remainder;
	result.min = safeMuldiv32(value.min, multiplier, divisor.max, remainder);
	result.openmin = remainder != 0 || value.openmin || divisor.openmax;
	if (divisor.min > 0) {
		result.max = safeMuldiv32(value.max, multiplier, divisor.min, remainder);
		if (remainder != 0) {
			result.max++;
			result.openmax = 1;
		} else {
			result.openmax = value.openmax || divisor.openmin;
		}
	} else {
		result.max = UINT32_MAX;
		result.openmax = 0;
	}

	return result;
}

constexpr snd_interval intervalMulDiv(const snd_interval &value, const snd_interval &multiplier, uint32_t divisor) {
	snd_interval result{};

	if (value.empty || multiplier.empty) {
		result.empty = 1;
		return result;
	}

	uint32_t remainder;
	result.min = safeMuldiv32(value.min, multiplier.min, divisor, remainder);
	result.openmin = remainder != 0 || value.openmin || multiplier.openmin;
	result.max = safeMuldiv32(value.max, multiplier.max, divisor, remainder);
	if (remainder != 0) {
		result.max++;
		result.openmax = 1;
	} else {
		result.openmax = value.openmax || multiplier.openmax;
	}

	return result;
}

constexpr snd_interval intervalStep(snd_interval interval, uint32_t step) {
	uint32_t misalign = interval.min % step;
	if (misalign != 0 || interval.openmin) {
		interval.min += step - misalign;
		interval.openmin = 0;
	}

	misalign = interval.max % step;
	if (misalign != 0 || interval.openmax) {
		interval.max -= misalign;
		interval.openmax = 0;
	}

	if (interval.min > interval.max)
		interval.empty = 1;

	return interval;
}

constexpr snd_interval intervalPow2(snd_interval interval) {
	if (!std::has_single_bit(interval.min) || interval.openmin) {
		interval.min = std::bit_ceil(interval.min + interval.openmin);
		interval.openmin = 0;
	}

	if (!std::has_single_bit(interval.max) || interval.openmax) {
		interval.max = std::bit_floor(interval.max - interval.openmax);
		interval.openmax = 0;
	}

	if (interval.min > interval.max)
		interval.empty = 1;

	return interval;
}

} // namespace utils

namespace rules {

extern std::array<Rule *, 19> commonRules;

} // namespace rules

} // namespace alsa
