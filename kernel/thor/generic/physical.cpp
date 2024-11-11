#include <assert.h>
#include <thor-internal/arch/paging.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/physical.hpp>

namespace thor {

static bool logPhysicalAllocs = false;

// --------------------------------------------------------
// PhysicalChunkAllocator
// --------------------------------------------------------

PhysicalChunkAllocator::PhysicalChunkAllocator() {
}

void PhysicalChunkAllocator::bootstrapRegion(PhysicalAddr address,
		int order, size_t numRoots, int8_t *buddyTree) {
	if(_numRegions >= 8) {
		infoLogger() << "thor: Ignoring memory region (can only handle 8 regions)"
				<< frg::endlog;
		return;
	}

	int n = _numRegions++;
	_allRegions[n].physicalBase = address;
	_allRegions[n].regionSize = numRoots << (order + kPageShift);
	_allRegions[n].buddyAccessor = BuddyAccessor{address, kPageShift,
			buddyTree, numRoots, order};

	auto currentTotal = _totalPages.load(std::memory_order_relaxed);
	auto currentFree = _freePages.load(std::memory_order_relaxed);
	_totalPages.store(currentTotal + (numRoots << order), std::memory_order_relaxed);
	_freePages.store(currentFree + (numRoots << order), std::memory_order_relaxed);
}

PhysicalAddr PhysicalChunkAllocator::allocate(size_t size, int addressBits) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto currentFree = _freePages.load(std::memory_order_relaxed);
	auto currentUsed = _usedPages.load(std::memory_order_relaxed);
	assert(currentFree > size / kPageSize);
	_freePages.store(currentFree - size / kPageSize, std::memory_order_relaxed);
	_usedPages.store(currentUsed + size / kPageSize, std::memory_order_relaxed);

	// TODO: This could be solved better.
	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;
	assert(size == (size_t(kPageSize) << target));

	if(logPhysicalAllocs)
		infoLogger() << "thor: Allocating physical memory of order "
					<< (target + kPageShift) << frg::endlog;
	for(int i = 0; i < _numRegions; i++) {
		if(target > _allRegions[i].buddyAccessor.tableOrder())
			continue;

		auto physical = _allRegions[i].buddyAccessor.allocate(target, addressBits);
		if(physical == BuddyAccessor::illegalAddress)
			continue;
	//	infoLogger() << "Allocate " << (void *)physical << frg::endlog;
		assert(!(physical % (size_t(kPageSize) << target)));
		return physical;
	}

	return static_cast<PhysicalAddr>(-1);
}

void PhysicalChunkAllocator::free(PhysicalAddr address, size_t size) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	
	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;

	for(int i = 0; i < _numRegions; i++) {
		if(address < _allRegions[i].physicalBase)
			continue;
		if(address + size - _allRegions[i].physicalBase > _allRegions[i].regionSize)
			continue;

		_allRegions[i].buddyAccessor.free(address, target);
		auto currentFree = _freePages.load(std::memory_order_relaxed);
		auto currentUsed = _usedPages.load(std::memory_order_relaxed);
		assert(currentUsed > size / kPageSize);
		_freePages.store(currentFree + size / kPageSize, std::memory_order_relaxed);
		_usedPages.store(currentUsed - size / kPageSize, std::memory_order_relaxed);
		return;
	}

	assert(!"Physical page is not part of any region");
}

} // namespace thor
