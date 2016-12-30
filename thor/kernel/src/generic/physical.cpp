
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

	// Now we proceed to initialize the buddy allocator.
	_buddyOrder = frigg::buddy_tools::suitable_order(_bootstrapLength >> kPageShift);

	// Subtract the buddy tree from the size of the chunk
	// and determine the number of roots for the buddy tree.
	auto pre_roots = _bootstrapLength >> (kPageShift + _buddyOrder);
	auto overhead = frigg::buddy_tools::determine_size(pre_roots, _buddyOrder) >> kPageShift;
	assert(overhead < _bootstrapLength);
	
	frigg::infoLogger() << "thor: Using an order "
			<< _buddyOrder << " buddy allocator"
			<< " (" << overhead << " pages overhead)"<< frigg::endLog;

	_buddyRoots = (_bootstrapLength - overhead) >> (kPageShift + _buddyOrder);
	assert(_buddyRoots >= 32);

	// Finally initialize the buddy tree.
	_physicalBase = _bootstrapBase + (overhead << kPageShift);
	_buddyPointer = reinterpret_cast<int8_t *>(physicalToVirtual(_bootstrapBase));
	frigg::buddy_tools::initialize(_buddyPointer, _buddyRoots, _buddyOrder);

	_freePages += (_bootstrapLength - overhead) >> kPageShift;
}

PhysicalAddr PhysicalChunkAllocator::allocate(Guard &guard, size_t size) {
	assert(guard.protects(&lock));

	assert(_freePages > size / kPageSize);
	_freePages -= size / kPageSize;
	_usedPages += size / kPageSize;

	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;
	assert(size == (size_t(kPageSize) << target));

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
	assert(size == (size_t(kPageSize) << target));

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

