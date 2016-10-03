
#include <frigg/cxx-support.hpp>
#include <frigg/support.hpp>

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

void *memcpy(void *dest, const void *src, size_t n) {
	for(size_t i = 0; i < n; i++)
		((char *)dest)[i] = ((const char *)src)[i];
	return dest;
}

void *memset(void *dest, int byte, size_t count) {
	for(size_t i = 0; i < count; i++)
		((char *)dest)[i] = (char)byte;
	return dest;
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

