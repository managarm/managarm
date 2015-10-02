
#ifndef FRIGG_C_SUPPORT_H
#define FRIGG_C_SUPPORT_H

#ifdef FRIGG_NO_LIBC

#include <frigg/types.hpp>
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

#elif !defined(FRIGG_HAVE_LIBC)
#error "Define either FRIGG_HAVE_LIBC or FRIGG_NO_LIBC"
#endif // FRIGG_NO_LIBC

#endif // FRIGG_C_SUPPORT_H

