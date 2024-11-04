#ifndef _ASSERT_H
#define _ASSERT_H

// Adapted from mlibc's assert.h.

#ifdef __cplusplus
extern "C" {
#endif

// NOTE: This is not ISO C. Declared in LSB.
void
__assert_fail(const char *assertion, const char *file, unsigned int line, const char *function);

#ifdef __cplusplus
}
#endif

#endif // _ASSERT_H

// NOTE: [7.2] requires this be outside the include guard
#ifdef NDEBUG

#undef assert
#define assert(ignore) ((void)0)

#else // NDEBUG

#undef assert
#define assert(assertion)                                                                          \
	((void)((assertion) || (__assert_fail(#assertion, __FILE__, __LINE__, __func__), 0)))

#endif // NDEBUG
