
#include "../../../frigg/include/types.hpp"
#include "../util/general.hpp"
#include "../runtime.hpp"
#include "physical-alloc.hpp"

namespace thor {
namespace memory {

PhysicalAllocator *tableAllocator;

// --------------------------------------------------------
// StupidPhysicalAllocator
// --------------------------------------------------------

StupidPhysicalAllocator::StupidPhysicalAllocator(uintptr_t next_page)
			: p_nextPage(next_page) { }

uintptr_t StupidPhysicalAllocator::allocate(size_t num_pages) {
	uintptr_t page = p_nextPage;
	p_nextPage += 0x1000 * num_pages;
	return page;
}

}} // namespace thor::memory

