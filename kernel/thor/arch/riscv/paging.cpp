#include <arch/variable.hpp>
#include <riscv/csr.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/paging.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/physical.hpp>

namespace thor {

namespace {

constinit frg::manual_box<KernelPageSpace> kernelSpace;
constinit frg::manual_box<EternalCounter> kernelSpaceCounter;
constinit frg::manual_box<smarter::shared_ptr<KernelPageSpace>> kernelSpacePtr;

} // namespace

// --------------------------------------------------------
// Physical page access.
// --------------------------------------------------------

void switchToPageTable(PhysicalAddr root, int asid, bool invalidate) { unimplementedOnRiscv(); }

void switchAwayFromPageTable(int asid) { unimplementedOnRiscv(); }

void invalidateAsid(int asid) { unimplementedOnRiscv(); }

void invalidatePage(int asid, const void *address) { unimplementedOnRiscv(); }

void initializeAsidContext(CpuData *cpuData) {
	auto irqLock = frg::guard(&irqMutex());

	cpuData->asidData.initialize(1);
	cpuData->asidData->globalBinding.initialize(globalBindingId);
	cpuData->asidData->globalBinding.initialBind(*kernelSpacePtr);
}

// --------------------------------------------------------
// Kernel page management.
// --------------------------------------------------------

void KernelPageSpace::initialize() {
	uint64_t satp = riscv::readCsr<riscv::Csr::satp>();
	uint64_t pml4 = (satp & ((UINT64_C(1) << 44) - 1)) << 12;
	kernelSpace.initialize(pml4);

	// Construct an eternal smart_ptr to the kernel page space for global bindings.
	kernelSpaceCounter.initialize();
	kernelSpacePtr.initialize(smarter::adopt_rc, kernelSpace.get(), kernelSpaceCounter.get());
}

KernelPageSpace &KernelPageSpace::global() { return *kernelSpace; }

KernelPageSpace::KernelPageSpace(PhysicalAddr satp) : PageSpace{satp} {}

void KernelPageSpace::mapSingle4k(
    VirtualAddr pointer, PhysicalAddr physical, uint32_t flags, CachingMode cachingMode
) {
	assert(!(pointer & (kPageSize - 1)));
	assert(!(physical & (kPageSize - 1)));

	Cursor cursor{this, pointer};
	cursor.map4k(physical, flags, cachingMode);
}

PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) { unimplementedOnRiscv(); }

// --------------------------------------------------------
// User page management.
// --------------------------------------------------------

ClientPageSpace::ClientPageSpace() : PageSpace{physicalAllocator->allocate(kPageSize)} {
	assert(rootTable() != PhysicalAddr(-1) && "OOM");

	// Initialize the bottom half to unmapped memory.
	PageAccessor accessor{rootTable()};
	auto tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor.get());

	for (size_t i = 0; i < 256; i++)
		tbl4[i].store(0);

	// Share the top half with the kernel.
	PageAccessor kernelAccessor{KernelPageSpace::global().rootTable()};
	auto kernelTbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(kernelAccessor.get());

	for (size_t i = 256; i < 512; i++) {
		auto pte = kernelTbl4[i].load();
		assert(pte & pteValid);
		tbl4[i].store(pte);
	}
}

ClientPageSpace::~ClientPageSpace() { unimplementedOnRiscv(); }

bool ClientPageSpace::updatePageAccess(VirtualAddr pointer) { unimplementedOnRiscv(); }

} // namespace thor