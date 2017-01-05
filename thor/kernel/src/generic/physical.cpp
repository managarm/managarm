
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// PhysicalChunkAllocator
// --------------------------------------------------------

PhysicalChunkAllocator::PhysicalChunkAllocator() {
	_buddyPointer = reinterpret_cast<int8_t *>(0xFFFF'FF00'0000'0000);
}

void PhysicalChunkAllocator::bootstrap(PhysicalAddr address, int order, size_t num_roots) {
	_physicalBase = address;
	_buddyOrder = order;
	_buddyRoots = num_roots;

	_usedPages = 0;
	_freePages = _buddyRoots << _buddyOrder;
	frigg::infoLogger() << "Number of available pages: " << _freePages << frigg::endLog;
}

PhysicalAddr PhysicalChunkAllocator::allocate(size_t size) {
	auto lock = frigg::guard(&_mutex);

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
//	frigg::infoLogger() << "Allocate " << (void *)physical << frigg::endLog;
	assert(!(physical % (size_t(kPageSize) << target)));
	return physical;
}

void PhysicalChunkAllocator::free(PhysicalAddr address, size_t size) {
	auto lock = frigg::guard(&_mutex);
	
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

