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

void switchToPageTable(PhysicalAddr root, int asid, bool invalidate) {
	uint64_t mode = 8 + (ClientCursorPolicy::numLevels() - 3);
	riscv::writeCsr<riscv::Csr::satp>((root >> 12) | (mode << 60));
	asm volatile("sfence.vma" : : : "memory"); // This is too coarse (also invalidates global).
}

void switchAwayFromPageTable(int asid) { unimplementedOnRiscv(); }

void invalidateAsid(int asid) {
	asm volatile("sfence.vma" : : : "memory"); // This is too coarse (also invalidates global).
}

void invalidatePage(int asid, const void *address) {
	asm volatile("sfence.vma" : : : "memory"); // This is too coarse (also invalidates global).
}

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

PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	Cursor cursor{this, pointer};
	auto [_, addr] = cursor.unmap4k();
	return addr;
}

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

ClientPageSpace::~ClientPageSpace() {
	if (riscvConfigNote->numPtLevels == 3) {
		freePt<ClientCursorPolicy, 2, /*LowerHalfOnly=*/true>(rootTable());
	} else {
		assert(riscvConfigNote->numPtLevels == 4);
		freePt<ClientCursorPolicy, 3, /*LowerHalfOnly=*/true>(rootTable());
	}
}

bool ClientPageSpace::updatePageAccess(VirtualAddr pointer, PageFlags flags) {
	Cursor cursor{this, pointer};
	auto ptePtr = cursor.getPtePtr();
	if (!ptePtr)
		return false;
	auto pte = __atomic_load_n(ptePtr, __ATOMIC_RELAXED);
	if (!(pte & pteValid))
		return false;
	assert(pte & pteRead);

	uint64_t bits{0};
	// Reads are always valid.
	if (flags & page_access::read)
		bits |= pteAccess;
	if ((flags & page_access::execute) && (pte & pteExecute))
		bits |= pteAccess;
	if ((flags & page_access::write) && (pte & pteWrite))
		bits |= pteAccess | pteDirty;

	// Mask out bits that are already set (such that the return value is accurate).
	bits &= ~(pte & (pteAccess | pteDirty));
	if (!bits)
		return false;
	__atomic_fetch_or(ptePtr, bits, __ATOMIC_RELAXED);
	return true;
}

} // namespace thor
