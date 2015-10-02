
#ifndef FRIGG_ALGORITHM_HPP
#define FRIGG_ALGORITHM_HPP

#include <frigg/traits.hpp>

namespace frigg {

template<typename T>
void swap(T &a, T &b) {
	T temp(move(a));
	a = move(b);
	b = move(temp);
}

} // namespace frigg

#endif // FRIGG_ALGORITHM_HPP

