
namespace frigg {
namespace util {

template<typename T>
void swap(T &a, T &b) {
	T temp(traits::move(a));
	a = traits::move(b);
	b = traits::move(temp);
}

} } // namespace frigg::util

