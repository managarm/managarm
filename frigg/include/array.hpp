
namespace frigg {
namespace util {

template<typename T, int n>
class Array {
public:
	T &operator[] (int i) {
		return p_array[i];
	}

private:
	T p_array[n];
};

} } // namespace frigg::util

