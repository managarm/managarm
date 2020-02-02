#pragma once

#include "types.hpp"
#include <physical-buddy.hpp>

namespace thor {

struct SkeletalRegion {
public:
	static void initialize();

	static SkeletalRegion &global();

	// TODO: make this private
	SkeletalRegion() = default;

	SkeletalRegion(const SkeletalRegion &other) = delete;
	
	SkeletalRegion &operator= (const SkeletalRegion &other) = delete;

	void *access(PhysicalAddr physical);
};

class PhysicalChunkAllocator {
	typedef frigg::TicketLock Mutex;
public:
	PhysicalChunkAllocator();
	
	void bootstrap(PhysicalAddr address,
			int order, size_t num_roots, int8_t *buddy_tree);

	PhysicalAddr allocate(size_t size, int addressBits = 64);
	void free(PhysicalAddr address, size_t size);

	size_t numUsedPages();
	size_t numFreePages();

private:
	Mutex _mutex;

	PhysicalAddr _physicalBase;
	int8_t *_buddyPointer;
	int _buddyOrder;
	size_t _buddyRoots;
	BuddyAccessor _buddyAccessor;

	size_t _usedPages;
	size_t _freePages;
};

extern frigg::LazyInitializer<PhysicalChunkAllocator> physicalAllocator;

} // namespace thor
