
#ifndef FRIGG_ARRAY_HPP
#define FRIGG_ARRAY_HPP

#include <frigg/macros.hpp>

namespace frigg FRIGG_VISIBILITY {

template<typename T, int n>
class Array {
public:
	T &operator[] (int i) {
		return p_array[i];
	}

private:
	T p_array[n];
};

} // namespace frigg

#endif // FRIGG_ARRAY_HPP
