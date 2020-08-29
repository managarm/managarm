#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Functions required by GCC.
void *memset(void *, int, size_t);
void *memcpy(void *__restrict, const void *__restrict, size_t);
void *memmove(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);

// Other functions that are nice to have.
size_t strlen(const char *s);

#ifdef __cplusplus
}
#endif
