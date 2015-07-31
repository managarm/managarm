
namespace frigg {
namespace memory {

template<typename T, typename Allocator, typename... Args>
T *construct(Allocator &allocator, Args &&... args) {
	void *pointer = allocator.allocate(sizeof(T));
	return new(pointer) T(traits::forward<Args>(args)...);
}
template<typename T, typename Allocator, typename... Args>
T *constructN(Allocator &allocator, size_t n, Args &&... args) {
	T *pointer = (T *)allocator.allocate(sizeof(T) * n);
	for(size_t i = 0; i < n; i++)
		new(&pointer[i]) T(traits::forward<Args>(args)...);
	return pointer;
}

template<typename T, typename Allocator>
void destruct(Allocator &allocator, T *pointer) {
	allocator.free(pointer);
}

} } // namespace frigg::memory

