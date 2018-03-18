
#ifndef FRIGG_ARRAY_HPP
#define FRIGG_ARRAY_HPP

#include <frigg/macros.hpp>

namespace frigg FRIGG_VISIBILITY {

template<typename T, int n>
class Array {
public:
	T *data() {
		return _array;
	}

	T &operator[] (int i) {
		return _array[i];
	}

private:
	T _array[n];
};

} // namespace frigg

#endif // FRIGG_ARRAY_HPP
