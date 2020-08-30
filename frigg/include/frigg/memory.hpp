
#ifndef FRIGG_MEMORY_HPP
#define FRIGG_MEMORY_HPP

#include <frigg/macros.hpp>
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>

namespace frigg FRIGG_VISIBILITY {

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

template<typename T, typename Allocator, typename... Args>
T *construct(Allocator &allocator, Args &&... args) {
	void *pointer = allocator.allocate(sizeof(T));
	return new(pointer) T(forward<Args>(args)...);
}
template<typename T, typename Allocator, typename... Args>
T *constructN(Allocator &allocator, size_t n, Args &&... args) {
	T *pointer = (T *)allocator.allocate(sizeof(T) * n);
	for(size_t i = 0; i < n; i++)
		new(&pointer[i]) T(forward<Args>(args)...);
	return pointer;
}

template<typename T, typename Allocator>
void destruct(Allocator &allocator, T *pointer) {
	if(!pointer)
		return;
	pointer->~T();
	allocator.deallocate(pointer, sizeof(T));
}

template<typename T, typename Allocator>
void destructN(Allocator &allocator, T *pointer, size_t n) {
	if(!pointer)
		return;
	for(size_t i = 0; i < n; i++)
		pointer[i].~T();
	allocator.deallocate(pointer, sizeof(T) * n);
}

} // namespace frigg

#endif // FRIGG_MEMORY_HPP

