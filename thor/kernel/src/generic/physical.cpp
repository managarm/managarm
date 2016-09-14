
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// PhysicalChunkAllocator
// --------------------------------------------------------

PhysicalChunkAllocator::PhysicalChunkAllocator(PhysicalAddr bootstrap_base,
		size_t bootstrap_length)
: _bootstrapBase(bootstrap_base), _bootstrapLength(bootstrap_length),
		_usedPages(0), _freePages(0) {
	assert((bootstrap_base % 0x1000) == 0);
	assert((bootstrap_length % 0x1000) == 0);
}

void PhysicalChunkAllocator::bootstrap() {
	size_t fine_shift = kPageShift, coarse_shift = kPageShift + 8;
	size_t overhead = frigg::BuddyAllocator::computeOverhead(_bootstrapLength,
			fine_shift, coarse_shift);
	
	uintptr_t base = _bootstrapBase + overhead;
	size_t length = _bootstrapLength - overhead;

	// align the base to the next coarse boundary.
	uintptr_t misalign = base % (uintptr_t(1) << coarse_shift);
	if(misalign) {
		base += (uintptr_t(1) << coarse_shift) - misalign;
		length -= misalign;
	}

	// shrink the length to the next coarse boundary.
	length -= length % (size_t(1) << coarse_shift);

	_buddy.addChunk(base, length, fine_shift, coarse_shift,
			physicalToVirtual(_bootstrapBase));
	_freePages += length / kPageSize;
}

PhysicalAddr PhysicalChunkAllocator::allocate(Guard &guard, size_t size) {
	assert(guard.protects(&lock));
	
	assert(_freePages > 0);
	_freePages -= size / kPageSize;
	_usedPages += size / kPageSize;

	return _buddy.allocate(size);
}

void PhysicalChunkAllocator::free(Guard &guard, PhysicalAddr address) {
	assert(guard.protects(&lock));
	//assert(address >= p_root->baseAddress);
	//assert(address < p_root->baseAddress
	//		+ p_root->pageSize * p_root->numPages);
	
	assert(!"Fix free()");
	
//	assert(_usedPages > 0);
//	_usedPages -= size / kPageSize;
//	_freePages += size / kPageSize;
}

size_t PhysicalChunkAllocator::numUsedPages() {
	return _usedPages;
}
size_t PhysicalChunkAllocator::numFreePages() {
	return _freePages;
}

} // namespace thor

