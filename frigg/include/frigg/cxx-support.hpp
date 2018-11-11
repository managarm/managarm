
#ifndef FRIGG_CXX_SUPPORT_HPP
#define FRIGG_CXX_SUPPORT_HPP

#include <new>
#include <frigg/macros.hpp>
#include <frigg/c-support.h>

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

