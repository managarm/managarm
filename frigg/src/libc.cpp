#include <stddef.h>
#include <stdint.h>

extern "C" {

int isspace(int ch) {
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

int isprint(int ch) {
	return ch >= 0x20 && ch <= 0x7E;
}

int isupper(int ch) {
	return (ch >= 'A' && ch <= 'Z');
}

int isalpha(int ch) {
	return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

int isdigit(int ch) {
	return ch >= '0' && ch <= '9';
}

int isxdigit(int ch) {
	if(ch >= 'a' && ch <= 'f')
		return 1;
	if(ch >= 'A' && ch <= 'F')
		return 1;
	return isdigit(ch);
}

int toupper(int ch) {
	if(ch >= 'a' && ch <= 'z')
		return ch - 'a' + 'A';
	return ch;
}

int tolower(int ch) {
	if(ch >= 'A' && ch <= 'Z')
		return ch + 'A' + 'a';
	return ch;
}

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

// this function is copied from newlib and available under a BSD license
unsigned long strtoul(const char *nptr, char **endptr, int base) {
	// note: we assume that (unsigned long)-1 == ULONG_MAX
	const unsigned long ULONG_MAX = -1;

	const unsigned char *s = (const unsigned char *)nptr;
	unsigned long acc;
	int c;
	unsigned long cutoff;
	int neg = 0, any, cutlim;

	do {
		c = *s++;
	} while (isspace(c));
	if (c == '-') {
		neg = 1;
		c = *s++;
	} else if (c == '+')
		c = *s++;
	if ((base == 0 || base == 16) &&
	    c == '0' && (*s == 'x' || *s == 'X')) {
		c = s[1];
		s += 2;
		base = 16;
	}
	if (base == 0)
		base = c == '0' ? 8 : 10;
	cutoff = (unsigned long)ULONG_MAX / (unsigned long)base;
	cutlim = (unsigned long)ULONG_MAX % (unsigned long)base;
	for (acc = 0, any = 0;; c = *s++) {
		if (isdigit(c))
			c -= '0';
		else if (isalpha(c))
			c -= isupper(c) ? 'A' - 10 : 'a' - 10;
		else
			break;
		if (c >= base)
			break;
               if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
			any = -1;
		else {
			any = 1;
			acc *= base;
			acc += c;
		}
	}
	if (any < 0) {
		acc = (unsigned long)ULONG_MAX;
	} else if (neg)
		acc = -acc;
	if (endptr != 0)
		*endptr = (char *) (any ? (char *)s - 1 : nptr);
	return (acc);
}

size_t strlen(const char *str) {
	size_t length = 0;
	while(*str++ != 0)
		length++;
	return length;
}

int strcmp(const char *lhs, const char *rhs) {
	while(true) {
		if(*lhs == 0 && *rhs == 0)
			return 0;
		if(*lhs == 0 && *rhs != 0)
			return -1;
		if(*lhs != 0 && *rhs == 0)
			return 1;
		if(*lhs < *rhs)
			return -1;
		if(*lhs > *rhs)
			return 1;
		lhs++; rhs++;
	}
}

int strncmp(const char *lhs, const char *rhs, size_t count) {
	for(size_t i = 0; i < count; i++) {
		if(*lhs == 0 && *rhs == 0)
			return 0;
		if(*lhs == 0 && *rhs != 0)
			return -1;
		if(*lhs != 0 && *rhs == 0)
			return 1;
		if(*lhs < *rhs)
			return -1;
		if(*lhs > *rhs)
			return 1;
		lhs++; rhs++;
	}
	return 0;
}

char *strcpy(char *dest, const char *src) {
	while(true) {
		*dest = *src;
		if(*src == 0)
			break;
		dest++; src++;
	}
	return dest;
}

char *strncpy(char *dest, const char *src, size_t count) {
	for(size_t i = 0; i < count; i++) {
		*dest = *src;
		if(*src == 0)
			break;
		dest++; src++;
	}
	return dest;
}

char *strcat(char *dest, const char *src) {
	strcpy(dest + strlen(dest), src);
	return dest;
}

// --------------------------------------------------------------------------------------
// memcpy() implementation.
// --------------------------------------------------------------------------------------

// GCC and Clang both recognize __builtin_memcpy() even with no optimizations.
// If a compiler doesn't do this and translates to memcpy(), this will fail horribly.
namespace {
	extern "C++" {

	template<typename T>
	[[gnu::always_inline, gnu::artificial]]
	inline T alias_load(const unsigned char *&p) {
		T value;
		__builtin_memcpy(&value, p, sizeof(T));
		p += sizeof(T);
		return value;
	}

	template<typename T>
	[[gnu::always_inline, gnu::artificial]]
	inline void alias_store(unsigned char *&p, T value) {
		__builtin_memcpy(p, &value, sizeof(T));
		p += sizeof(T);
	}

	} // extern "C++"
}

#ifdef __LP64__

void *memcpy(void *dest, const void *src, size_t n) {
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

void *memset(void *dest, int byte, size_t n) {
	auto curDest = reinterpret_cast<unsigned char *>(dest);

	uint64_t pattern = static_cast<uint64_t>(byte)
			| (static_cast<uint64_t>(byte) << 8)
			| (static_cast<uint64_t>(byte) << 16)
			| (static_cast<uint64_t>(byte) << 24)
			| (static_cast<uint64_t>(byte) << 32)
			| (static_cast<uint64_t>(byte) << 40)
			| (static_cast<uint64_t>(byte) << 48)
			| (static_cast<uint64_t>(byte) << 56);

	while(n >= 8 * 8) {
		alias_store<uint64_t>(curDest, pattern);
		alias_store<uint64_t>(curDest, pattern);
		alias_store<uint64_t>(curDest, pattern);
		alias_store<uint64_t>(curDest, pattern);
		alias_store<uint64_t>(curDest, pattern);
		alias_store<uint64_t>(curDest, pattern);
		alias_store<uint64_t>(curDest, pattern);
		alias_store<uint64_t>(curDest, pattern);
		n -= 8 * 8;
	}
	if(n >= 4 * 8) {
		alias_store<uint64_t>(curDest, pattern);
		alias_store<uint64_t>(curDest, pattern);
		alias_store<uint64_t>(curDest, pattern);
		alias_store<uint64_t>(curDest, pattern);
		n -= 4 * 8;
	}
	if(n >= 2 * 8) {
		alias_store<uint64_t>(curDest, pattern);
		alias_store<uint64_t>(curDest, pattern);
		n -= 2 * 8;
	}
	if(n >= 8) {
		alias_store<uint64_t>(curDest, pattern);
		n -= 8;
	}
	if(n >= 4) {
		alias_store<uint32_t>(curDest, pattern);
		n -= 4;
	}
	if(n >= 2) {
		alias_store<uint16_t>(curDest, pattern);
		n -= 2;
	}
	if(n)
		*curDest = pattern;
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
