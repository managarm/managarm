
#include <frigg/types.hpp>
#include <frigg/libc.hpp>

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

size_t strlen(const char *str) {
	size_t length = 0;
	while(*str++ != 0)
		length++;
	return length;
}

