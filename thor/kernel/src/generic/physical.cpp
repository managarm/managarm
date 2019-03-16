
#include "kernel.hpp"

namespace thor {

static bool logPhysicalAllocs = false;

// --------------------------------------------------------
// SkeletalRegion
// --------------------------------------------------------

frigg::LazyInitializer<SkeletalRegion> skeletalSingleton;

void SkeletalRegion::initialize() {
	skeletalSingleton.initialize();
}

SkeletalRegion &SkeletalRegion::global() {
	return *skeletalSingleton;
}

void *SkeletalRegion::access(PhysicalAddr physical) {
	assert(!(physical & (kPageSize - 1)));
	return reinterpret_cast<void *>(0xFFFF'8000'0000'0000 + physical);
}

// --------------------------------------------------------
// PhysicalChunkAllocator
// --------------------------------------------------------

PhysicalChunkAllocator::PhysicalChunkAllocator() {
}

void PhysicalChunkAllocator::bootstrap(PhysicalAddr address,
		int order, size_t num_roots, int8_t *buddy_tree) {
	_physicalBase = address;
	_buddyOrder = order;
	_buddyRoots = num_roots;
	_buddyPointer = buddy_tree;

	_usedPages = 0;
	_freePages = _buddyRoots << _buddyOrder;
	frigg::infoLogger() << "Number of available pages: " << _freePages << frigg::endLog;
}

PhysicalAddr PhysicalChunkAllocator::allocate(size_t size) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(_freePages > size / kPageSize);
	_freePages -= size / kPageSize;
	_usedPages += size / kPageSize;

	// TODO: This could be solved better.
	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;
	assert(size == (size_t(kPageSize) << target));

	if(logPhysicalAllocs)
		frigg::infoLogger() << "thor: Allocating physical memory of order "
					<< (target + kPageShift) << frigg::endLog;
	auto physical = _physicalBase + (frigg::buddy_tools::allocate(_buddyPointer,
			_buddyRoots, _buddyOrder, target) << kPageShift);
//	frigg::infoLogger() << "Allocate " << (void *)physical << frigg::endLog;
	assert(!(physical % (size_t(kPageSize) << target)));
	return physical;
}

void PhysicalChunkAllocator::free(PhysicalAddr address, size_t size) {
	auto irq_lock = frigg::guard(&irqMutex());
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
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	return _usedPages;
}
size_t PhysicalChunkAllocator::numFreePages() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	return _freePages;
}

} // namespace thor

