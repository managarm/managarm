#include <arch/variable.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/paging.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/physical.hpp>

namespace thor {

// --------------------------------------------------------
// Physical page access.
// --------------------------------------------------------

void switchToPageTable(PhysicalAddr root, int asid, bool invalidate) { unimplementedOnRiscv(); }

void switchAwayFromPageTable(int asid) { unimplementedOnRiscv(); }

void invalidateAsid(int asid) { unimplementedOnRiscv(); }

void invalidatePage(int asid, const void *address) { unimplementedOnRiscv(); }

// --------------------------------------------------------
// Kernel page management.
// --------------------------------------------------------

frg::manual_box<KernelPageSpace> kernelSpaceSingleton;

void KernelPageSpace::initialize() { unimplementedOnRiscv(); }
KernelPageSpace &KernelPageSpace::global() { return *kernelSpaceSingleton; }

KernelPageSpace::KernelPageSpace(PhysicalAddr satp) : PageSpace{satp} {}

void KernelPageSpace::mapSingle4k(
    VirtualAddr pointer, PhysicalAddr physical, uint32_t flags, CachingMode caching_mode
) {
	unimplementedOnRiscv();
}

PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) { unimplementedOnRiscv(); }

// --------------------------------------------------------
// User page management.
// --------------------------------------------------------

ClientPageSpace::ClientPageSpace()

    : PageSpace{physicalAllocator->allocate(kPageSize)} {
	unimplementedOnRiscv();
}
ClientPageSpace::~ClientPageSpace() { unimplementedOnRiscv(); }

bool ClientPageSpace::updatePageAccess(VirtualAddr pointer) { unimplementedOnRiscv(); }

} // namespace thor
