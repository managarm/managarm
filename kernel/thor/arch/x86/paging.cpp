
#include <arch/variable.hpp>
#include <frg/list.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/arch-generic/paging.hpp>

// --------------------------------------------------------
// Physical page access.
// --------------------------------------------------------

namespace thor {

void switchToPageTable(PhysicalAddr root, int asid, bool invalidate) {
	auto cr3 = root | asid;
	if(getCpuData()->havePcids && !invalidate)
		cr3 |= PhysicalAddr(1) << 63; // Do not invalidate the PCID.
	asm volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

void switchAwayFromPageTable(int asid) {
	// Switch to the kernel CR3 and invalidate the PCID.
	auto cr3 = KernelPageSpace::global().rootTable() | asid;
	asm volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

void invalidateAsid(int asid) {
	assert(asid != globalBindingId);
	if(!getCpuData()->havePcids) {
		assert(asid == globalBindingId || asid == 0);

		uint64_t pml4;
		asm volatile ("mov %%cr3, %0" : "=r"(pml4));
		asm volatile ("mov %0, %%cr3" : : "r"(pml4) : "memory");
	} else {
		struct {
			uint64_t pcid;
			const void *address;
		} descriptor;

		descriptor.pcid = asid;
		descriptor.address = nullptr;

		uint64_t type = 1;
		asm volatile ("invpcid %1, %0" : : "r"(type), "m"(descriptor) : "memory");
	}
}

void invalidatePage(int asid, const void *address) {
	if(asid == globalBindingId || !getCpuData()->havePcids) {
		assert(asid == globalBindingId || asid == 0);

		auto p = reinterpret_cast<const char *>(address);
		asm volatile ("invlpg %0" : : "m"(*p) : "memory");
	} else {
		struct {
			uint64_t pcid;
			const void *address;
		} descriptor;

		descriptor.pcid = asid;
		descriptor.address = address;

		uint64_t type = 0;
		asm volatile ("invpcid %1, %0" : : "r"(type), "m"(descriptor) : "memory");
	}
}

// --------------------------------------------------------
// Kernel paging management.
// --------------------------------------------------------

namespace {

frg::manual_box<KernelPageSpace> kernelSpace;

frg::manual_box<EternalCounter> kernelSpaceCounter;
frg::manual_box<smarter::shared_ptr<KernelPageSpace>> kernelSpacePtr;

} // namespace anonymous

void KernelPageSpace::initialize() {
	PhysicalAddr pml4_ptr;
	asm volatile ("mov %%cr3, %0" : "=r" (pml4_ptr));

	kernelSpace.initialize(pml4_ptr);

	// Construct an eternal smart_ptr to the kernel page space for
	// global bindings.
	kernelSpaceCounter.initialize();
	kernelSpacePtr.initialize(
		smarter::adopt_rc,
		kernelSpace.get(),
		kernelSpaceCounter.get());
}

KernelPageSpace &KernelPageSpace::global() {
	return *kernelSpace;
}

// TODO(qookie): Can't we raise this?
static constexpr int maxPcidCount = 8;

void initializeAsidContext(CpuData *cpuData) {
	auto irqLock = frg::guard(&irqMutex());

	// If PCIDs are not supported, create only one binding.
	auto pcidCount = getCpuData()->havePcids ? maxPcidCount : 1;

	cpuData->asidData.initialize(pcidCount);
	cpuData->asidData->globalBinding.initialize(globalBindingId);
	cpuData->asidData->globalBinding.initialBind(*kernelSpacePtr);
}


KernelPageSpace::KernelPageSpace(PhysicalAddr pml4_address)
: PageSpace{pml4_address} { }

void KernelPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
		uint32_t flags, CachingMode cachingMode) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);

	Cursor cursor{this, pointer};
	cursor.map4k(physical, flags, cachingMode);
}

PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert((pointer % 0x1000) == 0);

	Cursor cursor{this, pointer};
	auto [_, addr] = cursor.unmap4k();

	return addr;
}

// --------------------------------------------------------
// ClientPageSpace
// --------------------------------------------------------

ClientPageSpace::ClientPageSpace()
: PageSpace{physicalAllocator->allocate(kPageSize)} {
	assert(rootTable() != PhysicalAddr(-1) && "OOM");

	// Initialize the bottom half to unmapped memory.
	PageAccessor accessor{rootTable()};
	auto tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor.get());

	for(size_t i = 0; i < 256; i++)
		tbl4[i].store(0);

	// Share the top half with the kernel.
	PageAccessor kernelAccessor{KernelPageSpace::global().rootTable()};
	auto kernelTbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(kernelAccessor.get());

	for(size_t i = 256; i < 512; i++) {
		auto pte = kernelTbl4[i].load();
		assert(pte & ptePresent);
		tbl4[i].store(pte);
	}
}

ClientPageSpace::~ClientPageSpace() {
	auto clearLevel2 = [&] (PhysicalAddr ps) {
		PageAccessor accessor{ps};
		auto tbl = reinterpret_cast<uint64_t *>(accessor.get());
		for(int i = 0; i < 512; i++) {
			if(tbl[i] & ptePresent)
				physicalAllocator->free(tbl[i] & pteAddress, kPageSize);
		}
	};

	auto clearLevel3 = [&] (PhysicalAddr ps) {
		PageAccessor accessor{ps};
		auto tbl = reinterpret_cast<uint64_t *>(accessor.get());
		for(int i = 0; i < 512; i++) {
			if(!(tbl[i] & ptePresent))
				continue;
			clearLevel2(tbl[i] & pteAddress);
			physicalAllocator->free(tbl[i] & pteAddress, kPageSize);
		}
	};

	// From PML4, we only clear the lower half (higher half is shared with kernel).
	PageAccessor root_accessor{rootTable()};
	auto root_tbl = reinterpret_cast<uint64_t *>(root_accessor.get());
	for(int i = 0; i < 256; i++) {
		if(!(root_tbl[i] & ptePresent))
			continue;
		clearLevel3(root_tbl[i] & pteAddress);
		physicalAllocator->free(root_tbl[i] & pteAddress, kPageSize);
	}

	physicalAllocator->free(rootTable(), kPageSize);
}

bool ClientPageSpace::updatePageAccess(VirtualAddr, PageFlags) {
	return false;
}

} // namespace thor
