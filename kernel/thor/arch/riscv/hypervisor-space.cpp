#include <thor-internal/arch/hypervisor-space.hpp>
#include <thor-internal/arch/paging.hpp>

namespace thor::riscv_hypervisor {

// TODO: The root table is expanded to 16KB allowing 2 more address bits,
// the current code only uses the first 4KB.
using HypervisorCursorPolicy = ClientCursorPolicy;
using HypervisorCursor = PageCursor<HypervisorCursorPolicy>;

using Operations = HypervisorSpace::Operations;

HypervisorPageSpace::HypervisorPageSpace(PhysicalAddr root) : PageSpace{root} {}

HypervisorPageSpace::~HypervisorPageSpace() {
	if (riscvConfigNote->numPtLevels == 3) {
		freePt<HypervisorCursorPolicy, 2, /*LowerHalfOnly=*/false, 0x4000>(rootTable());
	} else {
		assert(riscvConfigNote->numPtLevels == 4);
		freePt<HypervisorCursorPolicy, 3, /*LowerHalfOnly=*/false, 0x4000>(rootTable());
	}
}

Operations::Operations(HypervisorPageSpace *pageSpace) : pageSpace_{pageSpace} {}

void Operations::retire(RetireNode *node) {
	asm volatile("hfence.gvma");
	node->complete();
}

bool Operations::submitShootdown(ShootNode *node) {
	for (VirtualAddr i = node->address; i < node->address + node->size; i += kPageSize) {
		asm volatile("hfence.gvma %0" : : "r"(i >> 2));
	}

	node->complete();
	return false;
}

frg::expected<Error, PagesAffected> Operations::mapPresentPages(
    VirtualAddr va,
    MemoryView *view,
    uintptr_t offset,
    size_t size,
    PageFlags flags,
    CachingMode mode
) {
	return mapPresentPagesByCursor<HypervisorCursor>(
	    pageSpace_, va, view, offset, size, flags, mode
	);
}

frg::expected<Error, PagesAffected>
Operations::restrictPages(VirtualAddr va, size_t size, PageFlags flags, CachingMode mode) {
	return restrictPagesByCursor<HypervisorCursor>(pageSpace_, va, size, flags, mode);
}

frg::expected<Error, PagesAffected> Operations::faultPage(
    VirtualAddr va,
    MemoryView *view,
    uintptr_t offset,
    FetchFlags fetchFlags,
    PageFlags flags,
    CachingMode mode
) {
	return faultPageByCursor<HypervisorCursor>(
	    pageSpace_, va, view, offset, fetchFlags, flags, mode
	);
}

frg::expected<Error, PagesAffected> Operations::cleanPages(VirtualAddr va, size_t size) {
	return cleanPagesByCursor<HypervisorCursor>(pageSpace_, va, size);
}

frg::expected<Error, PagesAffected> Operations::unmapPages(VirtualAddr va, size_t size) {
	return unmapPagesByCursor<HypervisorCursor>(pageSpace_, va, size);
}

frg::expected<Error, PagesAffected> Operations::agePages(VirtualAddr va, size_t size, bool vacate) {
	return agePagesByCursor<HypervisorCursor>(pageSpace_, va, size, vacate);
}

HypervisorSpace::HypervisorSpace(CtorToken, PhysicalAddr root)
: VirtualizedPageSpace{&ops_},
  pageSpace_{root},
  ops_{&pageSpace_} {
	assert(root % 0x4000 == 0);
}

} // namespace thor::riscv_hypervisor
