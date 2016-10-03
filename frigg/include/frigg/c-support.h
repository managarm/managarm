
#ifndef FRIGG_C_SUPPORT_H
#define FRIGG_C_SUPPORT_H

#pragma GCC visibility push(default)

#include <stddef.h>
#include <stdint.h>

#pragma GCC visibility pop

#ifdef FRIGG_NO_LIBC

#include <frigg/libc.hpp>

#ifdef __cplusplus
extern "C" {
#endif

void __assert_fail(const char *assertion,
		const char *file, unsigned int line, const char *function);

#define assert(c) do { if(!(c)) __assert_fail(#c, __FILE__, __LINE__, __func__); } while(0)

#ifdef __cplusplus
}
#endif

#elif defined(FRIGG_HAVE_LIBC)

#pragma GCC visibility push(default)

#include <assert.h>
#include <string.h>

#pragma GCC visibility pop

#else
#error "Define either FRIGG_HAVE_LIBC or FRIGG_NO_LIBC"
#endif // FRIGG_NO_LIBC

#endif // FRIGG_C_SUPPORT_H

