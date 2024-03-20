#include <stddef.h>
#include <stdint.h>
#include <string.h>

// We need to define the true memcpy / memset symbols
// (string.h expands them to the __builtin variants).
#undef memcpy
#undef memset

extern "C" {

int memcmp(const void *lhs, const void *rhs, size_t count) {
	auto lhs_str = (const uint8_t *)lhs;
	auto rhs_str = (const uint8_t *)rhs;
	for(size_t i = 0; i < count; i++) {
		if(lhs_str[i] < rhs_str[i])
			return -1;
		if(lhs_str[i] > rhs_str[i])
			return 1;
	}
	return 0;
}

int strcmp(const char *lhs, const char *rhs) {
	auto lhs_bytes = (const uint8_t *)lhs;
	auto rhs_bytes = (const uint8_t *)rhs;

	size_t i = 0;

	while (lhs_bytes[i] && rhs_bytes[i]) {
		if (lhs_bytes[i] != rhs_bytes[i])
			return lhs_bytes[i] - rhs_bytes[i];

		i++;
	}

	return lhs_bytes[i] - rhs_bytes[i];
}

int strncmp(const char *lhs, const char *rhs, size_t count) {
	auto lhs_bytes = (const uint8_t *)lhs;
	auto rhs_bytes = (const uint8_t *)rhs;

	if (count == 0)
		return 0;

	size_t i = 0;

	while (lhs_bytes[i] && rhs_bytes[i] && i < count) {
		if (lhs_bytes[i] != rhs_bytes[i])
			return lhs_bytes[i] - rhs_bytes[i];

		i++;
	}

	if (i == count)
		i--;

	return lhs_bytes[i] - rhs_bytes[i];
}

size_t strlen(const char *str) {
	size_t length = 0;
	while(*str++ != 0)
		length++;
	return length;
}

// --------------------------------------------------------------------------------------
// memcpy() implementation.
// --------------------------------------------------------------------------------------

namespace {
	extern "C++" {

	template<typename T>
	struct word_helper {
		enum class [[gnu::may_alias, gnu::aligned(1)]] word_enum : T { };
	};

	template<typename T>
	using word = typename word_helper<T>::word_enum;

	template<typename T>
	[[gnu::always_inline, gnu::artificial]]
	inline word<T> alias_load(const unsigned char *&p) {
		word<T> value = *reinterpret_cast<const word<T> *>(p);
		p += sizeof(T);
		return value;
	}

	template<typename T>
	[[gnu::always_inline, gnu::artificial]]
	inline void alias_store(unsigned char *&p, word<T> value) {
		*reinterpret_cast<word<T> *>(p) = value;
		p += sizeof(T);
	}

	} // extern "C++"
}

#ifdef __LP64__

void *memcpy(void *__restrict dest, const void *__restrict src, size_t n) {
	auto curDest = reinterpret_cast<unsigned char *>(dest);
	auto curSrc = reinterpret_cast<const unsigned char *>(src);

	while(n >= 8 * 8) {
		auto w1 = alias_load<uint64_t>(curSrc);
		auto w2 = alias_load<uint64_t>(curSrc);
		auto w3 = alias_load<uint64_t>(curSrc);
		auto w4 = alias_load<uint64_t>(curSrc);
		auto w5 = alias_load<uint64_t>(curSrc);
		auto w6 = alias_load<uint64_t>(curSrc);
		auto w7 = alias_load<uint64_t>(curSrc);
		auto w8 = alias_load<uint64_t>(curSrc);
		alias_store<uint64_t>(curDest, w1);
		alias_store<uint64_t>(curDest, w2);
		alias_store<uint64_t>(curDest, w3);
		alias_store<uint64_t>(curDest, w4);
		alias_store<uint64_t>(curDest, w5);
		alias_store<uint64_t>(curDest, w6);
		alias_store<uint64_t>(curDest, w7);
		alias_store<uint64_t>(curDest, w8);
		n -= 8 * 8;
	}
	if(n >= 4 * 8) {
		auto w1 = alias_load<uint64_t>(curSrc);
		auto w2 = alias_load<uint64_t>(curSrc);
		auto w3 = alias_load<uint64_t>(curSrc);
		auto w4 = alias_load<uint64_t>(curSrc);
		alias_store<uint64_t>(curDest, w1);
		alias_store<uint64_t>(curDest, w2);
		alias_store<uint64_t>(curDest, w3);
		alias_store<uint64_t>(curDest, w4);
		n -= 4 * 8;
	}
	if(n >= 2 * 8) {
		auto w1 = alias_load<uint64_t>(curSrc);
		auto w2 = alias_load<uint64_t>(curSrc);
		alias_store<uint64_t>(curDest, w1);
		alias_store<uint64_t>(curDest, w2);
		n -= 2 * 8;
	}
	if(n >= 8) {
		auto w = alias_load<uint64_t>(curSrc);
		alias_store<uint64_t>(curDest, w);
		n -= 8;
	}
	if(n >= 4) {
		auto w = alias_load<uint32_t>(curSrc);
		alias_store<uint32_t>(curDest, w);
		n -= 4;
	}
	if(n >= 2) {
		auto w = alias_load<uint16_t>(curSrc);
		alias_store<uint16_t>(curDest, w);
		n -= 2;
	}
	if(n)
		*curDest = *curSrc;
	return dest;
}

#else // !__LP64__

void *memcpy(void *dest, const void *src, size_t n) {
	for(size_t i = 0; i < n; i++)
		((char *)dest)[i] = ((const char *)src)[i];
	return dest;
}

#endif // __LP64__ / !__LP64__

// --------------------------------------------------------------------------------------
// memset() implementation.
// --------------------------------------------------------------------------------------

#ifdef __LP64__

void *memset(void *dest, int val, size_t n) {
	auto curDest = reinterpret_cast<unsigned char *>(dest);
	unsigned char byte = val;

	// Get rid of misalignment.
	while(n && (reinterpret_cast<uintptr_t>(curDest) & 7)) {
		*curDest++ = byte;
		--n;
	}

	auto pattern64 = static_cast<word<uint64_t>>(
			static_cast<uint64_t>(byte)
			| (static_cast<uint64_t>(byte) << 8)
			| (static_cast<uint64_t>(byte) << 16)
			| (static_cast<uint64_t>(byte) << 24)
			| (static_cast<uint64_t>(byte) << 32)
			| (static_cast<uint64_t>(byte) << 40)
			| (static_cast<uint64_t>(byte) << 48)
			| (static_cast<uint64_t>(byte) << 56));

	auto pattern32 = static_cast<word<uint32_t>>(
			static_cast<uint32_t>(byte)
			| (static_cast<uint32_t>(byte) << 8)
			| (static_cast<uint32_t>(byte) << 16)
			| (static_cast<uint32_t>(byte) << 24));

	auto pattern16 = static_cast<word<uint16_t>>(
			static_cast<uint16_t>(byte)
			| (static_cast<uint16_t>(byte) << 8));

	while(n >= 8 * 8) {
		alias_store<uint64_t>(curDest, pattern64);
		alias_store<uint64_t>(curDest, pattern64);
		alias_store<uint64_t>(curDest, pattern64);
		alias_store<uint64_t>(curDest, pattern64);
		alias_store<uint64_t>(curDest, pattern64);
		alias_store<uint64_t>(curDest, pattern64);
		alias_store<uint64_t>(curDest, pattern64);
		alias_store<uint64_t>(curDest, pattern64);
		n -= 8 * 8;
	}
	if(n >= 4 * 8) {
		alias_store<uint64_t>(curDest, pattern64);
		alias_store<uint64_t>(curDest, pattern64);
		alias_store<uint64_t>(curDest, pattern64);
		alias_store<uint64_t>(curDest, pattern64);
		n -= 4 * 8;
	}
	if(n >= 2 * 8) {
		alias_store<uint64_t>(curDest, pattern64);
		alias_store<uint64_t>(curDest, pattern64);
		n -= 2 * 8;
	}
	if(n >= 8) {
		alias_store<uint64_t>(curDest, pattern64);
		n -= 8;
	}
	if(n >= 4) {
		alias_store<uint32_t>(curDest, pattern32);
		n -= 4;
	}
	if(n >= 2) {
		alias_store<uint16_t>(curDest, pattern16);
		n -= 2;
	}
	if(n)
		*curDest = byte;
	return dest;
}

#else // !__LP64__

void *memset(void *dest, int byte, size_t count) {
	for(size_t i = 0; i < count; i++)
		((char *)dest)[i] = (char)byte;
	return dest;
}

#endif // __LP64__ / !__LP64__

} // extern "C"
