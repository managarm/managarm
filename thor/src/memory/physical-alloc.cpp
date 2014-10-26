
#include "../../../frigg/include/arch_x86/types64.hpp"
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

uintptr_t StupidPhysicalAllocator::allocate() {
	uintptr_t page = p_nextPage;
	p_nextPage += 0x1000;
	return page;
}

}} // namespace thor::memory

