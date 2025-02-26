#include <thor-internal/arch/ept.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/physical.hpp>

namespace thor::vmx {

constexpr uint64_t eptRead = UINT64_C(1) << 0;
constexpr uint64_t eptWrite = UINT64_C(1) << 1;
constexpr uint64_t eptExecute = UINT64_C(1) << 2;
constexpr uint64_t eptCacheWb = UINT64_C(6) << 3;
constexpr uint64_t eptIgnorePat = UINT64_C(1) << 6;
constexpr uint64_t eptDirty = UINT64_C(1) << 9;
// TODO: Support user-executable permissions (needs VM-x bit).
// constexpr uint64_t eptUserExecute = UINT64_C(1) << 10;
constexpr uint64_t eptAddress = 0x000FFFFFFFFFF00;

struct EptCursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return 4; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & eptRead;
	}

	static constexpr PhysicalAddr ptePageAddress(uint64_t pte) {
		return pte & eptAddress;
	}

	static constexpr PageStatus ptePageStatus(uint64_t pte) {
		if(ptePagePresent(pte))
			return 0;
		PageStatus status = page_status::present;
		if(pte & eptDirty)
			status |= page_status::dirty;
		return status;
	}

	static PageStatus pteClean(uint64_t *ptePtr) {
		auto pte = __atomic_fetch_and(ptePtr, ~eptDirty, __ATOMIC_RELAXED);
		return ptePageStatus(pte);
	}

	static constexpr uint64_t pteBuild(PhysicalAddr physical, PageFlags flags, CachingMode cachingMode) {
		auto pte = physical | eptIgnorePat | eptRead; // TODO: Do not always set eptRead.

		if(flags & page_access::write)
			pte |= eptWrite;
		if(flags & page_access::execute)
			pte |= eptExecute;
		assert(cachingMode == CachingMode::null || cachingMode == CachingMode::writeBack);
		pte |= eptCacheWb;

		return pte;
	}


	static constexpr bool pteTablePresent(uint64_t pte) {
		return pte & eptRead;
	}

	static constexpr PhysicalAddr pteTableAddress(uint64_t pte) {
		return pte & eptAddress;
	}

	static uint64_t pteNewTable() {
		auto newPtAddr = physicalAllocator->allocate(kPageSize);
		assert(newPtAddr != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor{newPtAddr};
		memset(accessor.get(), 0, kPageSize);

		return newPtAddr | eptRead | eptWrite | eptExecute;
	}
};

static_assert(CursorPolicy<EptCursorPolicy>);

using EptCursor = thor::PageCursor<EptCursorPolicy>;

EptPageSpace::EptPageSpace(PhysicalAddr root)
: PageSpace{root} { }

EptPageSpace::~EptPageSpace() {
	freePt<EptCursorPolicy, 3>(rootTable());
}

EptOperations::EptOperations(EptPageSpace *pageSpace)
: pageSpace_{pageSpace} { }

void EptOperations::retire(RetireNode *node) {
	EptPtr ptr = {pageSpace_->rootTable(), 0};
	asm volatile (
		"invept (%0), %1;"
		: : "r"(&ptr), "r"((uint64_t)1)
	);
	node->complete();
}

bool EptOperations::submitShootdown(ShootNode *node) {
	EptPtr ptr = {pageSpace_->rootTable(), 0};
	asm volatile (
		"invept (%0), %1;"
		: : "r"(&ptr), "r"((uint64_t)1)
	);
	node->complete();
	return false;
}

frg::expected<Error> EptOperations::mapPresentPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size, PageFlags flags) {
	return mapPresentPagesByCursor<EptCursor>(pageSpace_,
			va, view, offset, size, flags);
}

frg::expected<Error> EptOperations::remapPresentPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size, PageFlags flags) {
	return remapPresentPagesByCursor<EptCursor>(pageSpace_,
			va, view, offset, size, flags);
}

frg::expected<Error> EptOperations::faultPage(VirtualAddr va, MemoryView *view,
		uintptr_t offset, PageFlags flags) {
	return faultPageByCursor<EptCursor>(pageSpace_,
			va, view, offset, flags);
}

frg::expected<Error> EptOperations::cleanPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size) {
	return cleanPagesByCursor<EptCursor>(pageSpace_,
			va, view, offset, size);
}

frg::expected<Error> EptOperations::unmapPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size) {
	return unmapPagesByCursor<EptCursor>(pageSpace_,
			va, view, offset, size);
}

Error EptSpace::map(uint64_t guestAddress, uint64_t hostAddress, int flags) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);

	PageAccessor spaceAccessor{spaceRoot};
	auto pml4e = reinterpret_cast<size_t*>(spaceAccessor.get());

	size_t pageFlags = 0;

	pageFlags |= (1 << EPT_READ);
	if(flags & 1) {
		pageFlags |= (1 << EPT_WRITE);
	}
	if(flags & 2) {
		pageFlags |= (1 << EPT_EXEC);
	}

	size_t* pdpte;
	if(!(pml4e[pml4eIdx] & (1 << EPT_READ))) {
		auto pdpte_ptr = physicalAllocator->allocate(kPageSize);
		if(reinterpret_cast<PhysicalAddr>(pdpte_ptr) == static_cast<PhysicalAddr>(-1)) {
			return Error::noMemory;
		}
		size_t entry = ((pdpte_ptr >> 12) << EPT_PHYSADDR) | pageFlags;
		pml4e[pml4eIdx] = entry;
		PageAccessor pdpteAccessor{pdpte_ptr};
		memset(pdpteAccessor.get(), 0, kPageSize);
		pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	} else {
		PageAccessor pdpteAccessor{(pml4e[pml4eIdx] >> EPT_PHYSADDR) << 12};
		pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}


	size_t* pde;
	if(!(pdpte[pdpteIdx] & (1 << EPT_READ))) {
		auto pdpte_ptr = physicalAllocator->allocate(kPageSize);
		if(reinterpret_cast<PhysicalAddr>(pdpte_ptr) == static_cast<PhysicalAddr>(-1)) {
			return Error::noMemory;
		}
		size_t entry = ((pdpte_ptr >> 12) << EPT_PHYSADDR) | pageFlags;
		pdpte[pdpteIdx] = entry;
		PageAccessor pdpteAccessor{pdpte_ptr};
		memset(pdpteAccessor.get(), 0, kPageSize);
		pde = reinterpret_cast<size_t*>(pdpteAccessor.get());
	} else {
		PageAccessor pdpteAccessor{(pdpte[pdpteIdx] >> EPT_PHYSADDR) << 12};
		pde = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}


	size_t* pte;
	if(!(pde[pdeIdx] & (1 << EPT_READ))) {
		auto pt_ptr = physicalAllocator->allocate(kPageSize);
		if(reinterpret_cast<PhysicalAddr>(pt_ptr) == static_cast<PhysicalAddr>(-1)) {
			return Error::noMemory;
		}
		size_t entry = ((pt_ptr >> 12) << EPT_PHYSADDR) | pageFlags;
		pde[pdeIdx] = entry;
		PageAccessor pdpteAccessor{pt_ptr};
		memset(pdpteAccessor.get(), 0, kPageSize);
		pte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	} else {
		PageAccessor pdpteAccessor{(pde[pdeIdx] >> EPT_PHYSADDR) << 12};
		pte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	auto alloc = hostAddress >> 12;
	size_t entry = (alloc << EPT_PHYSADDR) | pageFlags | (6 << EPT_MEMORY_TYPE) | (1 << EPT_IGNORE_PAT);
	pte[pteIdx] = entry | flags;

	return Error::success;
}

bool EptSpace::isMapped(VirtualAddr guestAddress) {
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);

	PageAccessor spaceAccessor{spaceRoot};
	size_t* pml4e = reinterpret_cast<size_t*>(spaceAccessor.get());

	size_t* pdpte;
	if(!(pml4e[pml4eIdx] & (1 << EPT_READ))) {
		return false;
	} else {
		PageAccessor pdpteAccessor{(pml4e[pml4eIdx] >> EPT_PHYSADDR) << 12};
		pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pde;
	if(!(pdpte[pdpteIdx] & (1 << EPT_READ))) {
		return false;
	} else {
		PageAccessor pdpteAccessor{(pdpte[pdpteIdx] >> EPT_PHYSADDR) << 12};
		pde = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pte;
	if(!(pde[pdeIdx] & (1 << EPT_READ))) {
		return false;
	} else {
		PageAccessor pdpteAccessor{(pde[pdeIdx] >> EPT_PHYSADDR) << 12};
		pte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	return ((pte[pteIdx] & EPT_READ));
}

uintptr_t EptSpace::translate(uintptr_t guestAddress) {
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);
	int offset = (size_t)guestAddress & (0x1000 - 1);

	PageAccessor spaceAccessor{spaceRoot};
	size_t* pml4e = reinterpret_cast<size_t*>(spaceAccessor.get());

	size_t* pdpte;
	if(!(pml4e[pml4eIdx] & (1 << EPT_READ))) {
		return -1;
	} else {
		PageAccessor pdpteAccessor{(pml4e[pml4eIdx] >> EPT_PHYSADDR) << 12};
		pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pde;
	if(!(pdpte[pdpteIdx] & (1 << EPT_READ))) {
		return -1;
	} else {
		PageAccessor pdpteAccessor{(pdpte[pdpteIdx] >> EPT_PHYSADDR) << 12};
		pde = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pte;
	if(!(pde[pdeIdx] & (1 << EPT_READ))) {
		return -1;
	} else {
		PageAccessor pdpteAccessor{(pde[pdeIdx] >> EPT_PHYSADDR) << 12};
		pte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	return ((pte[pteIdx] >> EPT_PHYSADDR) << 12) + offset;
}

PageStatus EptSpace::unmap(uint64_t guestAddress) {
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);

	PageAccessor spaceAccessor{spaceRoot};
	size_t* pml4e = reinterpret_cast<size_t*>(spaceAccessor.get());

	size_t* pdpte;
	if(!(pml4e[pml4eIdx] & (1 << EPT_READ))) {
		return 0;
	} else {
		PageAccessor pdpteAccessor{(pml4e[pml4eIdx] >> EPT_PHYSADDR) << 12};
		pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pde;
	if(!(pdpte[pdpteIdx] & (1 << EPT_READ))) {
		return 0;
	} else {
		PageAccessor pdpteAccessor{(pdpte[pdpteIdx] >> EPT_PHYSADDR) << 12};
		pde = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pte;
	if(!(pde[pdeIdx] & (1 << EPT_READ))) {
		return 0;
	} else {
		PageAccessor pdpteAccessor{(pde[pdeIdx] >> EPT_PHYSADDR) << 12};
		pte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	PageStatus status = page_status::present;
	if(pte[pteIdx] & (1 << 9)) {
		status |= page_status::dirty;
	}
	pte[pteIdx] = 0;
	return status;
}

Error EptSpace::store(uintptr_t guestAddress, size_t size, const void* buffer) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = guestAddress + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = translate(write - misalign);
		if(page == PhysicalAddr(-1)) {
			return Error::fault;
		}

		PageAccessor accessor{page};
		memcpy((char *)accessor.get() + misalign, (char *)buffer + progress, chunk);
		progress += chunk;
	}
	return Error::success;
}

Error EptSpace::load(uintptr_t guestAddress, size_t size, void* buffer) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = guestAddress + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = translate(write - misalign);
		if(page == PhysicalAddr(-1)) {
			return Error::fault;
		}

		PageAccessor accessor{page};
		memcpy((char *)buffer + progress, (char *)accessor.get() + misalign, chunk);
		progress += chunk;
	}
	return Error::success;
}

bool EptSpace::submitShootdown(ShootNode *node) {
	EptPtr ptr = {spaceRoot, 0};
	asm volatile (
		"invept (%0), %1;"
		: : "r"(&ptr), "r"((uint64_t)1)
	);
	node->complete();
	return false;
}

void EptSpace::retire(RetireNode *node) {
	EptPtr ptr = {spaceRoot, 0};
	asm volatile (
		"invept (%0), %1;"
		: : "r"(&ptr), "r"((uint64_t)1)
	);
	node->complete();
}

EptSpace::~EptSpace() {
	PageAccessor spaceAccessor{spaceRoot};
	auto pml4e = reinterpret_cast<size_t*>(spaceAccessor.get());

	for(int i = 0; i < 512; i++) {
		if(pml4e[i] & (1 << EPT_READ)) {
			PageAccessor pdpteAccessor{(pml4e[i] >> EPT_PHYSADDR) << 12};
			auto pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
			for(int j = 0; j < 512; j++) {
				if(pdpte[j] & (1 << EPT_READ)) {
					PageAccessor pdeAccessor{(pdpte[j] >> EPT_PHYSADDR) << 12};
					auto pde = reinterpret_cast<size_t*>(pdeAccessor.get());
					for(int k = 0; k < 512; k++) {
						if(pde[k] & (1 << EPT_READ)) {
							physicalAllocator->free((size_t)(pde[k] >> EPT_PHYSADDR) << 12, kPageSize);
						}
					}
					physicalAllocator->free((size_t)(pdpte[j] >> EPT_PHYSADDR) << 12, kPageSize);
				}
			}
			physicalAllocator->free((size_t)(pml4e[i] >> EPT_PHYSADDR) << 12, kPageSize);
		}
	}
	physicalAllocator->free(spaceRoot, kPageSize);
}

} // namespace thor
