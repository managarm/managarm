#pragma once

#include <stddef.h>

#define memset __builtin_memset
#define memcpy __builtin_memcpy

#ifdef __cplusplus
extern "C" {
#endif

// [7.24.2] Copying functions

// memcpy is defined as a macro above.
char *strcpy(char *__restrict, const char *);
char *strncpy(char *__restrict, const char *, size_t);

// [7.24.4] Comparison functions

int memcmp(const void *, const void *, size_t);

// [7.24.6] Miscellaneous functions

// memset is defined as a macro above.
size_t strlen(const char *);
size_t strnlen(const char *, size_t);

#ifdef __cplusplus
}
#endif
