
#include <arch/variable.hpp>
#include <frg/list.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/arch/paging.hpp>

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

void poisonPhysicalAccess(PhysicalAddr physical) {
	auto address = 0xFFFF'8000'0000'0000 + physical;
	KernelPageSpace::global().unmapSingle4k(address);
	invalidatePage(globalBindingId, reinterpret_cast<void *>(address));
}

void poisonPhysicalWriteAccess(PhysicalAddr physical) {
	auto address = 0xFFFF'8000'0000'0000 + physical;
	KernelPageSpace::global().unmapSingle4k(address);
	KernelPageSpace::global().mapSingle4k(address, physical, 0, CachingMode::null);
	invalidatePage(globalBindingId, reinterpret_cast<void *>(address));
}

} // namespace thor

// --------------------------------------------------------

enum {
	kPagePresent = 0x1,
	kPageWrite = 0x2,
	kPageUser = 0x4,
	kPagePwt = 0x8,
	kPagePcd = 0x10,
	kPageDirty = 0x40,
	kPagePat = 0x80,
	kPageGlobal = 0x100,
	kPageXd = 0x8000000000000000,
	kPageAddress = 0x000FFFFFFFFFF000
};

namespace thor {

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
	cpuData->asidData->globalBinding.rebind(*kernelSpacePtr);
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
	PageAccessor accessor;
	accessor = PageAccessor{rootTable()};
	auto tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor.get());

	for(size_t i = 0; i < 256; i++)
		tbl4[i].store(0);

	// Share the top half with the kernel.
	auto kernel_pml4 = KernelPageSpace::global().rootTable();
	auto kernel_table = (uint64_t *)SkeletalRegion::global().access(kernel_pml4);

	for(size_t i = 256; i < 512; i++) {
		assert(kernel_table[i] & kPagePresent);
		tbl4[i].store(kernel_table[i]);
	}
}

ClientPageSpace::~ClientPageSpace() {
	auto clearLevel2 = [&] (PhysicalAddr ps) {
		PageAccessor accessor{ps};
		auto tbl = reinterpret_cast<uint64_t *>(accessor.get());
		for(int i = 0; i < 512; i++) {
			if(tbl[i] & kPagePresent)
				physicalAllocator->free(tbl[i] & kPageAddress, kPageSize);
		}
	};

	auto clearLevel3 = [&] (PhysicalAddr ps) {
		PageAccessor accessor{ps};
		auto tbl = reinterpret_cast<uint64_t *>(accessor.get());
		for(int i = 0; i < 512; i++) {
			if(!(tbl[i] & kPagePresent))
				continue;
			clearLevel2(tbl[i] & kPageAddress);
			physicalAllocator->free(tbl[i] & kPageAddress, kPageSize);
		}
	};

	// From PML4, we only clear the lower half (higher half is shared with kernel).
	PageAccessor root_accessor{rootTable()};
	auto root_tbl = reinterpret_cast<uint64_t *>(root_accessor.get());
	for(int i = 0; i < 256; i++) {
		if(!(root_tbl[i] & kPagePresent))
			continue;
		clearLevel3(root_tbl[i] & kPageAddress);
		physicalAllocator->free(root_tbl[i] & kPageAddress, kPageSize);
	}

	physicalAllocator->free(rootTable(), kPageSize);
}

bool ClientPageSpace::updatePageAccess(VirtualAddr) {
	return false;
}

} // namespace thor
