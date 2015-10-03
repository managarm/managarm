
#ifndef FRIGG_MEMORY_SLAB_HPP
#define FRIGG_MEMORY_SLAB_HPP

namespace frigg {

inline int log2(uint64_t n) {
	#define S(k) if (n >= (uint64_t(1) << k)) { i += k; n >>= k; }
	int i = 0;
	S(32); S(16); S(8); S(4); S(2); S(1);
	return i;
	#undef S
}

template<typename VirtualAlloc, typename Mutex>
class SlabAllocator {
public:
	SlabAllocator(VirtualAlloc &virt_alloc);

	void *allocate(size_t length);
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
		
		AreaType type;
		uintptr_t baseAddress;
		size_t length;
		int power;
		size_t numAllocated;

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

	int power = log2(length);
	if(length > uintptr_t(1) << power)
		power++;
	if(power < kMinPower)
		power = kMinPower;
	
	if(power <= kMaxPower) {
		int index = power - kMinPower;
		if(p_freeList[index] == nullptr) {
			size_t area_size = uintptr_t(1) << kMaxPower;
			VirtualArea *area = allocateNewArea(kTypeSlab, area_size);
			fillSlabArea(area, power);
		}
		
		FreeChunk *chunk = p_freeList[index];
		assert(chunk != nullptr);
		p_freeList[index] = chunk->nextChunk;
		return chunk;
	}else{
		size_t area_size = length + (kPageSize - (length % kPageSize));
		VirtualArea *area = allocateNewArea(kTypeLarge, area_size);
		return (void *)area->baseAddress;
	}
}

template<typename VirtualAlloc, typename Mutex>
void SlabAllocator<VirtualAlloc, Mutex>::free(void *pointer) {
	LockGuard<Mutex> guard(&p_mutex);
	
	if(!pointer)
		return;

	uintptr_t address = (uintptr_t)pointer;

	VirtualArea *previous = nullptr;
	VirtualArea *current = p_root;
	while(current != nullptr) {
		if(address >= current->baseAddress
				&& address < current->baseAddress + current->length) {
			if(current->type == kTypeSlab) {
				int index = current->power - kMinPower;
				int item_size = uintptr_t(1) << current->power;
				assert(((address - current->baseAddress) % item_size) == 0);

				auto chunk = new (pointer) FreeChunk();
				chunk->nextChunk = p_freeList[index];
				p_freeList[index] = chunk;
				return;
			}else{
				assert(current->type == kTypeLarge);
				assert(address == current->baseAddress);
				
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
	size_t map_size = area_size + kVirtualAreaPadding;
	assert((map_size % kPageSize) == 0);
	uintptr_t address = p_virtAllocator.map(map_size);
	p_usedPages += map_size / kPageSize;
	
	// setup the virtual area descriptor
	uintptr_t area_base = address + kVirtualAreaPadding;
	auto area = new ((void *)address) VirtualArea(kTypeSlab,
			area_base, area_size);
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
	assert((area->length % item_size) == 0);

	int index = power - kMinPower;
	for(size_t i = 0; i < num_items; i++) {
		auto chunk = new ((void *)(area->baseAddress + i * item_size)) FreeChunk();
		chunk->nextChunk = p_freeList[index];
		p_freeList[index] = chunk;
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
: type(type), baseAddress(address), length(length), power(0), numAllocated(0),
		right(nullptr) { }

} // namespace frigg

#endif // FRIGG_MEMORY_SLAB_HPP

