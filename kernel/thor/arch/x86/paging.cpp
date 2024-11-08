
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
	}else if(caching_mode == CachingMode::uncached) {
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
		bool user_page, uint32_t flags, CachingMode caching_mode) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	PageAccessor accessor4;
	PageAccessor accessor3;
	PageAccessor accessor2;
	PageAccessor accessor1;

	arch::scalar_variable<uint64_t> *tbl4;
	arch::scalar_variable<uint64_t> *tbl3;
	arch::scalar_variable<uint64_t> *tbl2;
	arch::scalar_variable<uint64_t> *tbl1;

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);

	// The PML4 does always exist.
	accessor4 = PageAccessor{rootTable()};

	// Make sure there is a PDPT.
	tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor4.get());
	if(tbl4[index4].load() & kPagePresent) {
		accessor3 = PageAccessor{tbl4[index4].load() & 0x000FFFFFFFFFF000};
	}else{
		auto tbl_address = physicalAllocator->allocate(kPageSize);
		assert(tbl_address != PhysicalAddr(-1) && "OOM");
		accessor3 = PageAccessor{tbl_address};
		memset(accessor3.get(), 0, kPageSize);

		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		tbl4[index4].store(new_entry);
	}
	assert(user_page ? ((tbl4[index4].load() & kPageUser) != 0)
			: ((tbl4[index4].load() & kPageUser) == 0));

	// Make sure there is a PD.
	tbl3 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor3.get());
	if(tbl3[index3].load() & kPagePresent) {
		accessor2 = PageAccessor{tbl3[index3].load() & 0x000FFFFFFFFFF000};
	}else{
		auto tbl_address = physicalAllocator->allocate(kPageSize);
		assert(tbl_address != PhysicalAddr(-1) && "OOM");
		accessor2 = PageAccessor{tbl_address};
		memset(accessor2.get(), 0, kPageSize);

		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		tbl3[index3].store(new_entry);
	}
	assert(user_page ? ((tbl3[index3].load() & kPageUser) != 0)
			: ((tbl3[index3].load() & kPageUser) == 0));

	// Make sure there is a PT.
	tbl2 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor2.get());
	if(tbl2[index2].load() & kPagePresent) {
		accessor1 = PageAccessor{tbl2[index2].load() & 0x000FFFFFFFFFF000};
	}else{
		auto tbl_address = physicalAllocator->allocate(kPageSize);
		assert(tbl_address != PhysicalAddr(-1) && "OOM");
		accessor1 = PageAccessor{tbl_address};
		memset(accessor1.get(), 0, kPageSize);

		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		tbl2[index2].store(new_entry);
	}
	assert(user_page ? ((tbl2[index2].load() & kPageUser) != 0)
			: ((tbl2[index2].load() & kPageUser) == 0));

	// Setup the new PTE.
	tbl1 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor1.get());
	assert(!(tbl1[index1].load() & kPagePresent));
	uint64_t new_entry = physical | kPagePresent;
	if(user_page)
		new_entry |= kPageUser;
	if(flags & page_access::write)
		new_entry |= kPageWrite;
	if(!(flags & page_access::execute))
		new_entry |= kPageXd;
	if(caching_mode == CachingMode::writeThrough) {
		new_entry |= kPagePwt;
	}else if(caching_mode == CachingMode::writeCombine) {
		new_entry |= kPagePat | kPagePwt;
	}else if(caching_mode == CachingMode::uncached) {
		new_entry |= kPagePwt | kPagePcd | kPagePat;
	}else{
		assert(caching_mode == CachingMode::null || caching_mode == CachingMode::writeBack);
	}
	tbl1[index1].store(new_entry);
}

PageStatus ClientPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	PageAccessor accessor4;
	PageAccessor accessor3;
	PageAccessor accessor2;
	PageAccessor accessor1;

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);

	// The PML4 is always present.
	accessor4 = PageAccessor{rootTable()};
	auto tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor4.get());

	// Find the PDPT.
	if(!(tbl4[index4].load() & kPagePresent))
		return 0;
	assert(tbl4[index4].load() & kPagePresent);
	accessor3 = PageAccessor{tbl4[index4].load() & 0x000FFFFFFFFFF000};
	auto tbl3 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor3.get());

	// Find the PD.
	if(!(tbl3[index3].load() & kPagePresent))
		return 0;
	assert(tbl3[index3].load() & kPagePresent);
	accessor2 = PageAccessor{tbl3[index3].load() & 0x000FFFFFFFFFF000};
	auto tbl2 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor2.get());

	// Find the PT.
	if(!(tbl2[index2].load() & kPagePresent))
		return 0;
	assert(tbl2[index2].load() & kPagePresent);
	accessor1 = PageAccessor{tbl2[index2].load() & 0x000FFFFFFFFFF000};
	auto tbl1 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor1.get());

	// TODO: Do we want to preserve some bits?
	auto bits = tbl1[index1].atomic_exchange(0);
	if(!(bits & kPagePresent))
		return 0;

	PageStatus status = page_status::present;
	if(bits & kPageDirty)
		status |= page_status::dirty;
	return status;
}

PageStatus ClientPageSpace::cleanSingle4k(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	PageAccessor accessor4;
	PageAccessor accessor3;
	PageAccessor accessor2;
	PageAccessor accessor1;

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);

	// The PML4 is always present.
	accessor4 = PageAccessor{rootTable()};
	auto tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor4.get());

	// Find the PDPT.
	if(!(tbl4[index4].load() & kPagePresent))
		return 0;
	assert(tbl4[index4].load() & kPagePresent);
	accessor3 = PageAccessor{tbl4[index4].load() & 0x000FFFFFFFFFF000};
	auto tbl3 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor3.get());

	// Find the PD.
	if(!(tbl3[index3].load() & kPagePresent))
		return 0;
	assert(tbl3[index3].load() & kPagePresent);
	accessor2 = PageAccessor{tbl3[index3].load() & 0x000FFFFFFFFFF000};
	auto tbl2 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor2.get());

	// Find the PT.
	if(!(tbl2[index2].load() & kPagePresent))
		return 0;
	assert(tbl2[index2].load() & kPagePresent);
	accessor1 = PageAccessor{tbl2[index2].load() & 0x000FFFFFFFFFF000};
	auto tbl1 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor1.get());

	auto bits = tbl1[index1].load();
	if(!(bits & kPagePresent))
		return 0;
	PageStatus status = page_status::present;
	if(bits & kPageDirty) {
		status |= page_status::dirty;
		tbl1[index1].atomic_exchange(bits & ~kPageDirty);
	}
	return status;
}

bool ClientPageSpace::isMapped(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	PageAccessor accessor4;
	PageAccessor accessor3;
	PageAccessor accessor2;
	PageAccessor accessor1;

	arch::scalar_variable<uint64_t> *tbl4;
	arch::scalar_variable<uint64_t> *tbl3;
	arch::scalar_variable<uint64_t> *tbl2;
	arch::scalar_variable<uint64_t> *tbl1;

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);

	// The PML4 is always present.
	accessor4 = PageAccessor{rootTable()};
	tbl4 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor4.get());

	// Find the PDPT.
	if(!(tbl4[index4].load() & kPagePresent))
		return false;
	accessor3 = PageAccessor{tbl4[index4].load() & 0x000FFFFFFFFFF000};
	tbl3 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor3.get());

	// Find the PD.
	if(!(tbl3[index3].load() & kPagePresent))
		return false;
	accessor2 = PageAccessor{tbl3[index3].load() & 0x000FFFFFFFFFF000};
	tbl2 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor2.get());

	// Find the PT.
	if(!(tbl2[index2].load() & kPagePresent))
		return false;
	accessor1 = PageAccessor{tbl2[index2].load() & 0x000FFFFFFFFFF000};
	tbl1 = reinterpret_cast<arch::scalar_variable<uint64_t> *>(accessor1.get());

	return tbl1[index1].load() & kPagePresent;
}

bool ClientPageSpace::updatePageAccess(VirtualAddr) {
	return false;
}

void ClientPageSpace::Cursor::realizePts() {
	auto doRealize = [&] <int S> (PageAccessor &subPt, PageAccessor &pt,
			std::integral_constant<int, S>) {
		auto ptPtr = reinterpret_cast<uint64_t *>(pt.get())
				+ ((va_ >> S) & 0x1FF);
		auto ptEnt = __atomic_load_n(ptPtr, __ATOMIC_RELAXED);
		if(ptEnt & ptePresent) {
			subPt = PageAccessor{ptEnt & pteAddress};
			return;
		}

		PhysicalAddr subPtPage = physicalAllocator->allocate(kPageSize);
		assert(subPtPage != static_cast<PhysicalAddr>(-1) && "OOM");

		subPt = PageAccessor{subPtPage};
		for(int i = 0; i < 512; i++) {
			auto subPtPtr = reinterpret_cast<uint64_t *>(subPt.get()) + i;
			*subPtPtr = 0;
		}

		ptEnt = subPtPage | ptePresent | pteWrite | pteUser;
		__atomic_store_n(ptPtr, ptEnt, __ATOMIC_RELEASE);
	};

	auto realize3 = [&] {
		if(_accessor3) /* [[likely]] */
			return;
		doRealize(_accessor3, _accessor4, std::integral_constant<int, 39>{});
	};
	auto realize2 = [&] {
		if(_accessor2) /* [[likely]] */
			return;
		realize3();
		doRealize(_accessor2, _accessor3, std::integral_constant<int, 30>{});
	};

	// This function is called after cachePts() if not all PTs are present.
	assert(!_accessor1);

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&space_->_mutex);
	{
		realize2();
		doRealize(_accessor1, _accessor2, std::integral_constant<int, 21>{});
	}
}

} // namespace thor

