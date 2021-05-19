#pragma once

#include <stddef.h>

#define memset __builtin_memset
#define memcpy __builtin_memcpy

#ifdef __cplusplus
extern "C" {
#endif

// [7.24.3] Concatenation functions

char *strcat(char *__restrict, const char *__restrict);
char *strncat(char *__restrict, const char *__restrict, size_t);

// [7.24.2] Copying functions

// memcpy is defined as a macro above.
void *memmove(void *, const void *, size_t);
char *strcpy(char *__restrict, const char *);
char *strncpy(char *__restrict, const char *, size_t);

// [7.24.4] Comparison functions

int memcmp(const void *, const void *, size_t);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);

// [7.24.5] Search functions

char *strstr(const char *, const char *);

// [7.24.6] Miscellaneous functions

// memset is defined as a macro above.
size_t strlen(const char *);

#ifdef __cplusplus
}
#endif
