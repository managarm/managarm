
#ifndef FRIGG_MEMORY_HPP
#define FRIGG_MEMORY_HPP

#include <frigg/traits.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory-slab.hpp>

namespace frigg {

// --------------------------------------------------------
// DebugAllocator declarations
// --------------------------------------------------------

enum {
	kPageSize = 0x1000
}; // TODO: allow different page sizes

template<typename VirtualAlloc, typename Mutex>
class DebugAllocator {
public:
	struct Header {
		size_t numPages;
		uint8_t padding[32 - sizeof(size_t)];

		Header(size_t num_pages);
	};

	DebugAllocator(VirtualAlloc &virt_alloc);

	DebugAllocator(const DebugAllocator &) = delete;

	DebugAllocator &operator= (const DebugAllocator &) = delete;

	static_assert(sizeof(Header) == 32, "Header is not 32 bytes long");

	void *allocate(size_t length);
	void free(void *pointer);

	size_t numUsedPages();

private:
	VirtualAlloc &p_virtualAllocator;
	Mutex p_mutex;

	size_t p_usedPages;
};

// --------------------------------------------------------
// DebugAllocator::Header definitions
// --------------------------------------------------------

template<typename VirtualAlloc, typename Mutex>
DebugAllocator<VirtualAlloc, Mutex>::Header::Header(size_t num_pages)
: numPages(num_pages) { }

// --------------------------------------------------------
// DebugAllocator definitions
// --------------------------------------------------------

template<typename VirtualAlloc, typename Mutex>
DebugAllocator<VirtualAlloc, Mutex>::DebugAllocator(VirtualAlloc &virt_alloc)
: p_virtualAllocator(virt_alloc), p_usedPages(0) { }

template<typename VirtualAlloc, typename Mutex>
void *DebugAllocator<VirtualAlloc, Mutex>::allocate(size_t length) {
	LockGuard<Mutex> guard(&p_mutex);

	size_t with_header = length + sizeof(Header);

	size_t num_pages = with_header / kPageSize;
	if((with_header % kPageSize) != 0)
		num_pages++;

	uintptr_t pointer = p_virtualAllocator.map(num_pages * kPageSize);
	Header *header = (Header *)pointer;
	new (header) Header(num_pages);
	
	p_usedPages += num_pages;

	return (void *)(pointer + sizeof(Header));
}

template<typename VirtualAlloc, typename Mutex>
void DebugAllocator<VirtualAlloc, Mutex>::free(void *pointer) {
	if(pointer == nullptr)
		return;

	LockGuard<Mutex> guard(&p_mutex);
	
	Header *header = (Header *)((uintptr_t)pointer - sizeof(Header));
	
	size_t num_pages = header->numPages;
	p_virtualAllocator.unmap((uintptr_t)header, num_pages * kPageSize);

	assert(p_usedPages >= num_pages);
	p_usedPages -= num_pages;
}

template<typename VirtualAlloc, typename Mutex>
size_t DebugAllocator<VirtualAlloc, Mutex>::numUsedPages() {
	return p_usedPages;
}

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
	allocator.free(pointer);
}

} // namespace frigg

#endif // FRIGG_MEMORY_HPP

