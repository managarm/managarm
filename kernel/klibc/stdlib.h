#ifndef THOR_LIBC_STDLIB_H
#define THOR_LIBC_STDLIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// [7.22.1] Numeric conversion functions

unsigned long strtoul(const char *__restrict, char **__restrict, int);

int vsnprintf(char *__restrict buf, size_t bufsz,
		const char *__restrict format, va_list vlist);
int snprintf(char *__restrict buf, size_t bufsz,
		const char *__restrict format, ... );

#ifdef __cplusplus
}
#endif

#endif // THOR_LIBC_STDLIB_H
