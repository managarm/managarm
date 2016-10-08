
#ifndef FRIGG_CXX_SUPPORT_HPP
#define FRIGG_CXX_SUPPORT_HPP

#include <frigg/macros.hpp>
#include <frigg/c-support.h>

#ifdef FRIGG_NO_LIBC

inline void *operator new (size_t size, void *pointer) {
	return pointer;
}

#elif defined(FRIGG_HAVE_LIBC)
#	include <new>
#else
#	error "Define either FRIGG_HAVE_LIBC or FRIGG_NO_LIBC"
#endif // FRIGG_NO_LIBC

namespace frigg FRIGG_VISIBILITY {

template<typename T>
const T &min(const T &a, const T &b) {
	return (b < a) ? b : a;
}

template<typename T>
const T &max(const T &a, const T &b) {
	return (a < b) ? b : a;
}

} // namespace frigg

#endif // FRIGG_CXX_SUPORT_HPP

