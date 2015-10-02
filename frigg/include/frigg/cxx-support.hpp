
#ifndef FRIGG_CXX_SUPPORT_HPP
#define FRIGG_CXX_SUPPORT_HPP

#include <frigg/c-support.h>

#ifdef FRIGG_NO_LIBC

inline void *operator new (size_t size, void *pointer) {
	return pointer;
}

#endif // FRIGG_NO_LIBC

#endif // FRIGG_CXX_SUPORT_HPP

