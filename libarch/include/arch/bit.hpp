#pragma once

#include <cstring>
#include <type_traits>

namespace arch {
template<typename...>
constexpr bool dependent_false = false;

enum class endian {
    little = __ORDER_LITTLE_ENDIAN__,
    big    = __ORDER_BIG_ENDIAN__,
    native = __BYTE_ORDER__
};

static_assert(endian::native == endian::little || endian::native == endian::big,
		"only little and big endian are supported");

template<typename T>
inline T bswap(T val) {
	static_assert(std::is_integral_v<T>, "T must be an integral type");
	if constexpr (sizeof(T) == 1) {
		return val;
	} else if constexpr (sizeof(T) == 2) {
		return __builtin_bswap16(val);
	} else if constexpr (sizeof(T) == 4) {
		return __builtin_bswap32(val);
	} else if constexpr (sizeof(T) == 8) {
		return __builtin_bswap64(val);
	} else {
		static_assert(dependent_false<T>, "unsupported swap size");
	}
}

template<endian NewEndian, endian OldEndian = endian::native, typename T>
inline T convert_endian(T native) {
	static_assert(std::is_integral_v<T>, "T must be an integral type");
	if constexpr (NewEndian != OldEndian) {
		return bswap(native);
	} else {
		return native;
	}
}
} // namespace arch
