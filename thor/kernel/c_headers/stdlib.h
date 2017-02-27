#ifndef THOR_LIBC_STDLIB_H
#define THOR_LIBC_STDLIB_H

#ifdef __cplusplus
extern "C" {
#endif

// [7.22.1] Numeric conversion functions

unsigned long strtoul(const char *__restrict, char **__restrict, int);

#ifdef __cplusplus
}
#endif

#endif // THOR_LIBC_STDLIB_H
