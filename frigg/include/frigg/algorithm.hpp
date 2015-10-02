
namespace frigg {

template<typename T>
void swap(T &a, T &b) {
	T temp(move(a));
	a = move(b);
	b = move(temp);
}

} // namespace frigg

