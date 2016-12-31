
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// PhysicalChunkAllocator
// --------------------------------------------------------

PhysicalChunkAllocator::PhysicalChunkAllocator(PhysicalAddr bootstrap_base,
		size_t bootstrap_length)
: _bootstrapBase(bootstrap_base), _bootstrapLength(bootstrap_length),
		_buddyPointer(nullptr), _usedPages(0), _freePages(0) {
	assert((bootstrap_base % 0x1000) == 0);
	assert((bootstrap_length % 0x1000) == 0);
}

void PhysicalChunkAllocator::bootstrap() {
	assert(!_buddyPointer);

	// Align the base pointer to 2 MiB (the size of a large page).
	// This ensures that we can hand out up to 2 MiB of contiguous physical memory.
	// TODO: Check for overflow.
	_physicalBase = _bootstrapBase;
	auto limit = _bootstrapBase + _bootstrapLength;
	_physicalBase = (_physicalBase + ((size_t(1) << 21) - 1)) & ~((size_t(1) << 21) - 1);
	assert(_physicalBase <= limit);

	// Now we proceed to initialize the buddy allocator.
	_buddyOrder = frigg::buddy_tools::suitable_order((limit - _physicalBase) >> kPageShift);

	// Subtract the buddy tree from the size of the chunk
	// and determine the number of roots for the buddy tree.
	auto pre_roots = (limit - _physicalBase) >> (kPageShift + _buddyOrder);
	auto overhead = frigg::buddy_tools::determine_size(pre_roots, _buddyOrder);
	assert(overhead < limit - _physicalBase);
	_buddyRoots = (limit - _physicalBase - overhead) >> (kPageShift + _buddyOrder);
	assert(_buddyRoots >= 32);

	// Finally initialize the buddy tree.
	auto table = (limit - overhead) & ~((size_t(1) << kPageShift) - 1);
	_buddyPointer = reinterpret_cast<int8_t *>(physicalToVirtual(table));
	frigg::buddy_tools::initialize(_buddyPointer, _buddyRoots, _buddyOrder);
	
	frigg::infoLogger() << "Memory region from " << (void *)_physicalBase
			<< " to " << (void *)limit << frigg::endLog;
	frigg::infoLogger() << "    Using an order " << _buddyOrder << " buddy allocator"
			<< " with " << _buddyRoots << " roots"
			<< " (" << (overhead >> kPageShift) << " pages overhead)"<< frigg::endLog;
	frigg::infoLogger() << "    Buddy table starts at " << (void *)table << frigg::endLog;

	_freePages += (limit - _physicalBase - overhead) >> kPageShift;
}

PhysicalAddr PhysicalChunkAllocator::allocate(Guard &guard, size_t size) {
	assert(guard.protects(&lock));

	assert(_freePages > size / kPageSize);
	_freePages -= size / kPageSize;
	_usedPages += size / kPageSize;

	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;
	if(size != (size_t(kPageSize) << target))
		frigg::infoLogger() << "\e[31mPhysical allocation of size " << (void *)size
				<< " rounded up to power of 2\e[39m" << frigg::endLog;

	auto physical = _physicalBase + (frigg::buddy_tools::allocate(_buddyPointer,
			_buddyRoots, _buddyOrder, target) << kPageShift);
	assert(!(physical % (size_t(kPageSize) << target)));
	return physical;
}

void PhysicalChunkAllocator::free(Guard &guard, PhysicalAddr address, size_t size) {
	assert(guard.protects(&lock));
	
	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;

	auto index = (address - _physicalBase) >> kPageShift;
	frigg::buddy_tools::free(_buddyPointer, _buddyRoots, _buddyOrder,
			index, target);
	
	assert(_usedPages > size / kPageSize);
	_freePages += size / kPageSize;
	_usedPages -= size / kPageSize;
}

size_t PhysicalChunkAllocator::numUsedPages() {
	return _usedPages;
}
size_t PhysicalChunkAllocator::numFreePages() {
	return _freePages;
}

} // namespace thor

