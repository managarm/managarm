
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
		uint32_t flags, CachingMode caching_mode) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto &region = SkeletalRegion::global();

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);

	// the pml4 exists already
	uint64_t *pml4_pointer = (uint64_t *)region.access(rootTable());

	// make sure there is a pdpt
	uint64_t pml4_initial_entry = pml4_pointer[pml4_index];
	uint64_t *pdpt_pointer;
	if((pml4_initial_entry & kPagePresent) != 0) {
		pdpt_pointer = (uint64_t *)region.access(pml4_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pdpt_page = physicalAllocator->allocate(kPageSize);
		assert(pdpt_page != static_cast<PhysicalAddr>(-1) && "OOM");

		pdpt_pointer = (uint64_t *)region.access(pdpt_page);
		for(int i = 0; i < 512; i++)
			pdpt_pointer[i] = 0;

		uint64_t new_entry = pdpt_page | kPagePresent | kPageWrite;
		pml4_pointer[pml4_index] = new_entry;
	}
	assert(!(pml4_pointer[pml4_index] & kPageUser));

	// make sure there is a pd
	uint64_t pdpt_initial_entry = pdpt_pointer[pdpt_index];
	uint64_t *pd_pointer;
	if((pdpt_initial_entry & kPagePresent) != 0) {
		pd_pointer = (uint64_t *)region.access(pdpt_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pd_page = physicalAllocator->allocate(kPageSize);
		assert(pd_page != static_cast<PhysicalAddr>(-1) && "OOM");

		pd_pointer = (uint64_t *)region.access(pd_page);
		for(int i = 0; i < 512; i++)
			pd_pointer[i] = 0;

		uint64_t new_entry = pd_page | kPagePresent | kPageWrite;
		pdpt_pointer[pdpt_index] = new_entry;
	}
	assert(!(pdpt_pointer[pdpt_index] & kPageUser));

	// make sure there is a pt
	uint64_t pd_initial_entry = pd_pointer[pd_index];
	uint64_t *pt_pointer;
	if((pd_initial_entry & kPagePresent) != 0) {
		pt_pointer = (uint64_t *)region.access(pd_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pt_page = physicalAllocator->allocate(kPageSize);
		assert(pt_page != static_cast<PhysicalAddr>(-1) && "OOM");

		pt_pointer = (uint64_t *)region.access(pt_page);
		for(int i = 0; i < 512; i++)
			pt_pointer[i] = 0;

		uint64_t new_entry = pt_page | kPagePresent | kPageWrite;
		pd_pointer[pd_index] = new_entry;
	}
	assert(!(pd_pointer[pd_index] & kPageUser));

	// setup the new pt entry
	assert((pt_pointer[pt_index] & kPagePresent) == 0);
	uint64_t new_entry = physical | kPagePresent | kPageGlobal;
	if(flags & page_access::write)
		new_entry |= kPageWrite;
	if(!(flags & page_access::execute))
		new_entry |= kPageXd;
	if(caching_mode == CachingMode::writeThrough) {
		new_entry |= kPagePwt;
	}else if(caching_mode == CachingMode::writeCombine) {
		new_entry |= kPagePat | kPagePwt;
	}else if(caching_mode == CachingMode::uncached || caching_mode == CachingMode::mmio
			|| caching_mode == CachingMode::mmioNonPosted) {
		new_entry |= kPagePwt | kPagePcd | kPagePat;
	}else{
		assert(caching_mode == CachingMode::null || caching_mode == CachingMode::writeBack);
	}
	pt_pointer[pt_index] = new_entry;
}

PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert((pointer % 0x1000) == 0);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto &region = SkeletalRegion::global();

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);

	// find the pml4_entry
	uint64_t *pml4_pointer = (uint64_t *)region.access(rootTable());
	uint64_t pml4_entry = pml4_pointer[pml4_index];

	// find the pdpt entry
	assert((pml4_entry & kPagePresent) != 0);
	uint64_t *pdpt_pointer = (uint64_t *)region.access(pml4_entry & 0x000FFFFFFFFFF000);
	uint64_t pdpt_entry = pdpt_pointer[pdpt_index];

	// find the pd entry
	assert((pdpt_entry & kPagePresent) != 0);
	uint64_t *pd_pointer = (uint64_t *)region.access(pdpt_entry & 0x000FFFFFFFFFF000);
	uint64_t pd_entry = pd_pointer[pd_index];

	// find the pt entry
	assert((pd_entry & kPagePresent) != 0);
	uint64_t *pt_pointer = (uint64_t *)region.access(pd_entry & 0x000FFFFFFFFFF000);

	// change the pt entry
	assert((pt_pointer[pt_index] & kPagePresent) != 0);
	pt_pointer[pt_index] ^= kPagePresent;
	return pt_pointer[pt_index] & 0x000FFFFFFFFFF000;
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

void ClientPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
		bool userPage, uint32_t flags, CachingMode cachingMode) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);
	assert(userPage); // !userPage only ever happens on AArch64

	Cursor cursor{this, pointer};
	cursor.map4k(physical, flags, cachingMode);
}

PageStatus ClientPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	Cursor cursor{this, pointer};
	return cursor.unmap4k();
}

PageStatus ClientPageSpace::cleanSingle4k(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	Cursor cursor{this, pointer};
	return cursor.clean4k();
}

bool ClientPageSpace::isMapped(VirtualAddr pointer) {
	// This is unimplemented because it's unused if cursors are
	// used by VirtualOperations. The definition for it is still
	// needed though as long as the non-cursor mapPresentPages
	// implementation is around.
	assert(!"Unimplemented");
}

bool ClientPageSpace::updatePageAccess(VirtualAddr) {
	return false;
}

} // namespace thor
