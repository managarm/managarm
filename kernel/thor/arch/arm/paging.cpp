#include <thor-internal/arch/paging.hpp>
#include <thor-internal/arch/ints.hpp>
#include <arch/variable.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

static inline constexpr uint64_t tlbiValue(uint16_t asid, uint64_t va = 0) {
	return (uint64_t(asid) << 48) | (va >> 12);
}

void invalidateAsid(int asid) {
	if(asid == globalBindingId) {
		asm volatile ("dsb st; \
			tlbi vmalle1; \
			dsb sy; isb"
			::: "memory");
	} else {
		asm volatile ("dsb st; \
			tlbi aside1, %0; \
			dsb sy; isb"
			:: "r"(tlbiValue(asid))
			: "memory");
	}
}

void invalidatePage(int asid, const void *address) {
	// Don't care whether asid == globalBindingId since for global
	// pages, as TLBI VAE1 invalidates global entries for any ASID.
	asm volatile ("dsb st;\n\t\
		tlbi vae1, %0;\n\t\
		dsb sy; isb"
		:: "r"(tlbiValue(asid, reinterpret_cast<uintptr_t>(address)))
		: "memory");
}

void switchToPageTable(PhysicalAddr root, int asid, bool invalidate) {
	assert(asid != globalBindingId);

	if(invalidate)
		invalidateAsid(asid);

	asm volatile("msr ttbr0_el1, %0; isb; dsb sy; isb"
		:: "r" ((uint64_t(asid) << 48) | root)
		: "memory");
}

namespace {

PhysicalAddr nullTable = PhysicalAddr(-1);

} // namespace anonymous

void switchAwayFromPageTable(int asid) {
	if(nullTable == PhysicalAddr(-1)) {
		nullTable = physicalAllocator->allocate(kPageSize);
		assert(nullTable != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor;
		accessor = PageAccessor{nullTable};
		auto l0 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor.get());

		for(size_t i = 0; i < 512; i++)
			l0[i].store(0);
	}

	switchToPageTable(nullTable, asid, true);
}

void poisonPhysicalAccess(PhysicalAddr physical) { assert(!"Not implemented"); }
void poisonPhysicalWriteAccess(PhysicalAddr physical) { assert(!"Not implemented"); }

namespace {

frg::manual_box<KernelPageSpace> kernelSpace;

frg::manual_box<EternalCounter> kernelSpaceCounter;
frg::manual_box<smarter::shared_ptr<KernelPageSpace>> kernelSpacePtr;

} // namespace anonymous

void KernelPageSpace::initialize() {
	PhysicalAddr ttbr1;
	asm volatile ("mrs %0, ttbr1_el1" : "=r" (ttbr1));

	kernelSpace.initialize(ttbr1);

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

void initializeAsidContext(CpuData *cpuData) {
	auto irqLock = frg::guard(&irqMutex());

	// TODO(qookie): Check the max number of ASIDs. 256 is safe, but it could also be 65536.
	cpuData->asidData.initialize(256);
	cpuData->asidData->globalBinding.initialize(globalBindingId);
	cpuData->asidData->globalBinding.rebind(*kernelSpacePtr);
}


KernelPageSpace::KernelPageSpace(PhysicalAddr ttbr1)
: PageSpace{ttbr1} { }

void KernelPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
		uint32_t flags, CachingMode caching_mode) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);

	auto ttbr = (pointer >> 63) & 1;
	assert(ttbr == 1);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto &region = SkeletalRegion::global();

	auto l0 = (pointer >> 39) & 0x1FF;
	auto l1 = (pointer >> 30) & 0x1FF;
	auto l2 = (pointer >> 21) & 0x1FF;
	auto l3 = (pointer >> 12) & 0x1FF;

	uint64_t *l0_ptr = (uint64_t *)region.access(rootTable() & 0xFFFFFFFFFFFE);
	uint64_t *l1_ptr = nullptr;
	uint64_t *l2_ptr = nullptr;
	uint64_t *l3_ptr = nullptr;

	auto l0_ent = l0_ptr[l0];
	if (!(l0_ent & kPageValid)) {
		PhysicalAddr page = physicalAllocator->allocate(kPageSize);
		assert(page != static_cast<PhysicalAddr>(-1) && "OOM");

		l1_ptr = (uint64_t *)region.access(page);

		for(int i = 0; i < 512; i++)
			l1_ptr[i] = 0;

		l0_ptr[l0] =
			page | kPageValid | kPageTable;
	} else {
		l1_ptr = (uint64_t *)region.access(l0_ent & 0xFFFFFFFFF000);
	}

	auto l1_ent = l1_ptr[l1];
	if (!(l1_ent & kPageValid)) {
		PhysicalAddr page = physicalAllocator->allocate(kPageSize);
		assert(page != static_cast<PhysicalAddr>(-1) && "OOM");

		l2_ptr = (uint64_t *)region.access(page);

		for(int i = 0; i < 512; i++)
			l2_ptr[i] = 0;

		l1_ptr[l1] =
			page | kPageValid | kPageTable;
	} else {
		l2_ptr = (uint64_t *)region.access(l1_ent & 0xFFFFFFFFF000);
	}

	auto l2_ent = l2_ptr[l2];
	if (!(l2_ent & kPageValid)) {
		PhysicalAddr page = physicalAllocator->allocate(kPageSize);
		assert(page != static_cast<PhysicalAddr>(-1) && "OOM");

		l3_ptr = (uint64_t *)region.access(page);

		for(int i = 0; i < 512; i++)
			l3_ptr[i] = 0;

		l2_ptr[l2] =
			page | kPageValid | kPageTable;
	} else {
		l3_ptr = (uint64_t *)region.access(l2_ent & 0xFFFFFFFFF000);
	}

	auto l3_ent = l3_ptr[l3];

	assert(!(l3_ent & kPageValid));

	uint64_t new_entry = physical | kPageValid | kPageL3Page | kPageAccess;

	if (!(flags & page_access::write))
		new_entry |= kPageRO;
	if (!(flags & page_access::execute))
		new_entry |= kPageXN | kPagePXN;

	if (caching_mode == CachingMode::writeCombine)
		new_entry |= kPageUc | kPageOuterSh;
	else if (caching_mode == CachingMode::uncached)
		new_entry |= kPagenGnRnE | kPageOuterSh;
	else if (caching_mode == CachingMode::mmio)
		new_entry |= kPagenGnRE | kPageOuterSh;
	else if (caching_mode == CachingMode::mmioNonPosted)
		new_entry |= kPagenGnRnE | kPageOuterSh;
	else {
		assert(caching_mode == CachingMode::null || caching_mode == CachingMode::writeBack);
		new_entry |= kPageWb | kPageInnerSh;
	}

	assert(!(new_entry & (uintptr_t(0b111) << 48)));

	l3_ptr[l3] = new_entry;
}

PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert((pointer % 0x1000) == 0);

	auto ttbr = (pointer >> 63) & 1;
	assert(ttbr == 1);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto &region = SkeletalRegion::global();

	auto l0 = (pointer >> 39) & 0x1FF;
	auto l1 = (pointer >> 30) & 0x1FF;
	auto l2 = (pointer >> 21) & 0x1FF;
	auto l3 = (pointer >> 12) & 0x1FF;

	uint64_t *l0_ptr = (uint64_t *)region.access(rootTable() & 0xFFFFFFFFFFFE);
	auto l0_ent = l0_ptr[l0];
	assert(l0_ent & kPageValid);

	uint64_t *l1_ptr = (uint64_t *)region.access(l0_ent & 0xFFFFFFFFF000);
	auto l1_ent = l1_ptr[l1];
	assert(l1_ent & kPageValid);

	uint64_t *l2_ptr = (uint64_t *)region.access(l1_ent & 0xFFFFFFFFF000);
	auto l2_ent = l2_ptr[l2];
	assert(l2_ent & kPageValid);

	uint64_t *l3_ptr = (uint64_t *)region.access(l2_ent & 0xFFFFFFFFF000);
	auto l3_ent = l3_ptr[l3];
	assert(l3_ent & kPageValid);

	l3_ptr[l3] ^= kPageValid;

	return l3_ent & 0xFFFFFFFFF000;
}

ClientPageSpace::ClientPageSpace()
: PageSpace{physicalAllocator->allocate(kPageSize)} {
	assert(rootTable() != PhysicalAddr(-1) && "OOM");

	PageAccessor accessor;
	accessor = PageAccessor{rootTable()};
	auto l0 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor.get());

	for(size_t i = 0; i < 512; i++)
		l0[i].store(0);
}

ClientPageSpace::~ClientPageSpace() {
	auto clearLevel2 = [&] (PhysicalAddr ps) {
		PageAccessor accessor{ps};
		auto tbl = reinterpret_cast<uint64_t *>(accessor.get());
		for(int i = 0; i < 512; i++) {
			if(tbl[i] & kPageValid)
				physicalAllocator->free(tbl[i] & kPageAddress, kPageSize);
		}
	};

	auto clearLevel1 = [&] (PhysicalAddr ps) {
		PageAccessor accessor{ps};
		auto tbl = reinterpret_cast<uint64_t *>(accessor.get());
		for(int i = 0; i < 512; i++) {
			if(!(tbl[i] & kPageValid))
				continue;
			clearLevel2(tbl[i] & kPageAddress);
			physicalAllocator->free(tbl[i] & kPageAddress, kPageSize);
		}
	};

	PageAccessor root_accessor{rootTable()};
	auto root_tbl = reinterpret_cast<uint64_t *>(root_accessor.get());
	for(int i = 0; i < 512; i++) {
		if(!(root_tbl[i] & kPageValid))
			continue;
		clearLevel1(root_tbl[i] & kPageAddress);
		physicalAllocator->free(root_tbl[i] & kPageAddress, kPageSize);
	}

	physicalAllocator->free(rootTable(), kPageSize);
}


// TODO(qookie): This function should be converted to use cursors.
// Maybe we should add a method that lets us just get a pointer to the
// PTE or nullptr if it's not mapped?
bool ClientPageSpace::updatePageAccess(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	PageAccessor accessor0;
	PageAccessor accessor1;
	PageAccessor accessor2;
	PageAccessor accessor3;

	arch::scalar_variable<uint64_t> *tbl0;
	arch::scalar_variable<uint64_t> *tbl1;
	arch::scalar_variable<uint64_t> *tbl2;
	arch::scalar_variable<uint64_t> *tbl3;

	auto index0 = (int)((pointer >> 39) & 0x1FF);
	auto index1 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index3 = (int)((pointer >> 12) & 0x1FF);

	accessor0 = PageAccessor{rootTable()};
	tbl0 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor0.get());

	if (tbl0[index0].load() & kPageValid) {
		accessor1 = PageAccessor{tbl0[index0].load() & kPageAddress};
		tbl1 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor1.get());
	} else {
		return false;
	}

	if (tbl1[index1].load() & kPageValid) {
		accessor2 = PageAccessor{tbl1[index1].load() & kPageAddress};
		tbl2 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor2.get());
	} else {
		return false;
	}

	if (tbl2[index2].load() & kPageValid) {
		accessor3 = PageAccessor{tbl2[index2].load() & kPageAddress};
		tbl3 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor3.get());
	} else {
		return false;
	}

	auto bits = tbl3[index3].load();
	if (!(bits & kPageValid))
		return false;

	if (!(bits & kPageRO) || !(bits & kPageShouldBeWritable))
		return false;

	bits &= ~kPageRO;
	tbl3[index3].store(bits);

	// Invalidate the page on the current CPU. No need for
	// shootdown, since at worst other CPUs will just run
	// updatePageAccess again themselves.
	PhysicalAddr ttbr0;
	asm volatile ("mrs %0, ttbr0_el1" : "=r" (ttbr0));
	auto asid = (ttbr0 >> 48) & 0xFFFF;

	invalidatePage(asid, reinterpret_cast<void *>(pointer));

	return true;
}

}
