
#ifndef FRIGG_MEMORY_SLAB_HPP
#define FRIGG_MEMORY_SLAB_HPP

#include <frigg/macros.hpp>
#include <frigg/debug.hpp>

namespace frigg FRIGG_VISIBILITY {

inline int nextPower(uint64_t n) {
	uint64_t u = n;

	#define S(k) if (u >= (uint64_t(1) << k)) { p += k; u >>= k; }
	int p = 0;
	S(32); S(16); S(8); S(4); S(2); S(1);
	#undef S

	if(n > (uint64_t(1) << p))
		p++;
	return p;
}

template<typename VirtualAlloc, typename Mutex>
class SlabAllocator {
public:
	SlabAllocator(VirtualAlloc &virt_alloc);

	void *allocate(size_t length);
	void *realloc(void *pointer, size_t new_length);
	void free(void *pointer);

	size_t numUsedPages() {
		return p_usedPages;
	}

private:
	enum {
		kPageSize = 0x1000,
		kVirtualAreaPadding = kPageSize,
		kMinPower = 5,
		kMaxPower = 16,
		kNumPowers = kMaxPower - kMinPower + 1
	};

	struct FreeChunk {
		FreeChunk *nextChunk;

		FreeChunk();
	};
	
	enum AreaType {
		kTypeNone,
		kTypeSlab,
		kTypeLarge
	};

	struct VirtualArea {
		VirtualArea(AreaType type, uintptr_t address, size_t length);
		
		const AreaType type;
		const uintptr_t baseAddress;
		const size_t length;

		int power;

		VirtualArea *right;
	};
	static_assert(sizeof(VirtualArea) <= kVirtualAreaPadding, "Padding too small");
	
	VirtualArea *allocateNewArea(AreaType type, size_t area_size);
	void fillSlabArea(VirtualArea *area, int power);

	VirtualArea *p_root;
	FreeChunk *p_freeList[kNumPowers];
	VirtualAlloc &p_virtAllocator;
	Mutex p_mutex;

	size_t p_usedPages;
};

// --------------------------------------------------------
// SlabAllocator
// --------------------------------------------------------

template<typename VirtualAlloc, typename Mutex>
SlabAllocator<VirtualAlloc, Mutex>::SlabAllocator(VirtualAlloc &virt_alloc)
: p_root(nullptr), p_virtAllocator(virt_alloc), p_usedPages(0) {
	for(size_t i = 0; i < kNumPowers; i++)
		p_freeList[i] = nullptr;
}

template<typename VirtualAlloc, typename Mutex>
void *SlabAllocator<VirtualAlloc, Mutex>::allocate(size_t length) {
	LockGuard<Mutex> guard(&p_mutex);
	
	if(length == 0)
		return nullptr;

	if(length <= (uintptr_t(1) << kMaxPower)) {
		int power = nextPower(length);
		assert(length <= (uintptr_t(1) << power));
		assert(power <= kMaxPower);
		if(power < kMinPower)
			power = kMinPower;
		
		int index = power - kMinPower;
		if(p_freeList[index] == nullptr) {
			size_t area_size = uintptr_t(1) << kMaxPower;
			VirtualArea *area = allocateNewArea(kTypeSlab, area_size);
			fillSlabArea(area, power);
		}
		
		FreeChunk *chunk = p_freeList[index];
		assert(chunk != nullptr);
		p_freeList[index] = chunk->nextChunk;
		//infoLogger() << "[slab] alloc " << chunk << frigg::endLog;
		return chunk;
	}else{
		size_t area_size = length;
		if((area_size % kPageSize) != 0)
			area_size += kPageSize - length % kPageSize;
		VirtualArea *area = allocateNewArea(kTypeLarge, area_size);
//		infoLogger() << "[" << (void *)area->baseAddress
//				<< "] Large alloc varea " << area << endLog;
		//infoLogger() << "[slab] alloc " << (void *)area->baseAddress << frigg::endLog;
		return (void *)area->baseAddress;
	}
}

template<typename VirtualAlloc, typename Mutex>
void *SlabAllocator<VirtualAlloc, Mutex>::realloc(void *pointer, size_t new_length) {
	if(!pointer) {
		return allocate(new_length);
	}else if(!new_length) {
		free(pointer);
		return nullptr;
	}

	LockGuard<Mutex> guard(&p_mutex);
	uintptr_t address = (uintptr_t)pointer;

	VirtualArea *current = p_root;
	while(current != nullptr) {
		if(address >= current->baseAddress
				&& address < current->baseAddress + current->length) {
			if(current->type == kTypeSlab) {
				size_t item_size = size_t(1) << current->power;

				if(new_length < item_size)
					return pointer;

				guard.unlock(); // TODO: this is inefficient
				void *new_pointer = allocate(new_length);
				if(!new_pointer)
					return nullptr;
				memcpy(new_pointer, pointer, item_size);
				free(pointer);
				//infoLogger() << "[slab] realloc " << new_pointer << frigg::endLog;
				return new_pointer;
			}else{
				assert(current->type == kTypeLarge);
				assert(address == current->baseAddress);
				
				if(new_length < current->length)
					return pointer;
				
				guard.unlock(); // TODO: this is inefficient
				void *new_pointer = allocate(new_length);
				if(!new_pointer)
					return nullptr;
				memcpy(new_pointer, pointer, current->length);
				free(pointer);
				//infoLogger() << "[slab] realloc " << new_pointer << frigg::endLog;
				return new_pointer;
			}
		}

		current = current->right;
	}

	assert(!"Pointer is not part of any virtual area");
	__builtin_unreachable();
}

template<typename VirtualAlloc, typename Mutex>
void SlabAllocator<VirtualAlloc, Mutex>::free(void *pointer) {
	LockGuard<Mutex> guard(&p_mutex);
	
	if(!pointer)
		return;
	//infoLogger() << "[slab] free " << pointer << frigg::endLog;

	uintptr_t address = (uintptr_t)pointer;

	VirtualArea *previous = nullptr;
	VirtualArea *current = p_root;
	while(current != nullptr) {
		if(address >= current->baseAddress
				&& address < current->baseAddress + current->length) {
			if(current->type == kTypeSlab) {
				int index = current->power - kMinPower;
				assert(current->power <= kMaxPower);
				size_t item_size = size_t(1) << current->power;
				assert(((address - current->baseAddress) % item_size) == 0);
//				infoLogger() << "[" << pointer
//						<< "] Small free from varea " << current << endLog;

				auto chunk = new (pointer) FreeChunk();
				chunk->nextChunk = p_freeList[index];
				p_freeList[index] = chunk;
				return;
			}else{
				assert(current->type == kTypeLarge);
				assert(address == current->baseAddress);
//				infoLogger() << "[" << pointer
//						<< "] Large free from varea " << current << endLog;
				
				// remove the virtual area from the area-list
				if(previous) {
					previous->right = current->right;
				}else{
					p_root = current->right;
				}
				
				// deallocate the memory used by the area
				p_usedPages -= (current->length + kVirtualAreaPadding) / kPageSize;
				p_virtAllocator.unmap((uintptr_t)current->baseAddress - kVirtualAreaPadding,
						current->length + kVirtualAreaPadding);
				return;
			}
		}
		
		previous = current;
		current = current->right;
	}

	assert(!"Pointer is not part of any virtual area");
}

template<typename VirtualAlloc, typename Mutex>
auto SlabAllocator<VirtualAlloc, Mutex>::allocateNewArea(AreaType type, size_t area_size)
-> VirtualArea * {
	// allocate virtual memory for the chunk
	assert((area_size % kPageSize) == 0);
	uintptr_t address = p_virtAllocator.map(area_size + kVirtualAreaPadding);
	p_usedPages += (area_size + kVirtualAreaPadding) / kPageSize;
	
	// setup the virtual area descriptor
	auto area = new ((void *)address) VirtualArea(type,
			address + kVirtualAreaPadding, area_size);
	area->right = p_root;
	p_root = area;

	return area;
}

template<typename VirtualAlloc, typename Mutex>
void SlabAllocator<VirtualAlloc, Mutex>::fillSlabArea(VirtualArea *area, int power) {
	assert(area->type == kTypeSlab && area->power == 0);
	area->power = power;

	// setup the free chunks in the new area
	size_t item_size = uintptr_t(1) << power;
	size_t num_items = area->length / item_size;
	assert(num_items > 0);
	assert((area->length % item_size) == 0);

	int index = power - kMinPower;
	for(size_t i = 0; i < num_items; i++) {
		auto chunk = new ((void *)(area->baseAddress + i * item_size)) FreeChunk();
		chunk->nextChunk = p_freeList[index];
		p_freeList[index] = chunk;
		//infoLogger() << "[slab] fill " << chunk << frigg::endLog;
	}
}

// --------------------------------------------------------
// SlabAllocator::FreeChunk
// --------------------------------------------------------

template<typename VirtualAlloc, typename Mutex>
SlabAllocator<VirtualAlloc, Mutex>::FreeChunk::FreeChunk()
: nextChunk(nullptr) { }

// --------------------------------------------------------
// SlabAllocator::VirtualArea
// --------------------------------------------------------

template<typename VirtualAlloc, typename Mutex>
SlabAllocator<VirtualAlloc, Mutex>::VirtualArea::VirtualArea(AreaType type,
		uintptr_t address, size_t length)
: type(type), baseAddress(address), length(length), power(0), right(nullptr) { }

} // namespace frigg

#endif // FRIGG_MEMORY_SLAB_HPP

