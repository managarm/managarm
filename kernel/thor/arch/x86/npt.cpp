#include <thor-internal/arch/npt.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/physical.hpp>

namespace thor::svm {

constexpr uint64_t nptPresent = UINT64_C(1) << 0;
constexpr uint64_t nptWrite = UINT64_C(1) << 1;
constexpr uint64_t nptUser = UINT64_C(1) << 2;
constexpr uint64_t nptDirty = UINT64_C(1) << 6;
constexpr uint64_t nptXd = UINT64_C(1) << 6;
constexpr uint64_t nptAddress = 0x000FFFFFFFFFF00;

struct NptCursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return 4; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & nptPresent;
	}

	static constexpr PhysicalAddr ptePageAddress(uint64_t pte) {
		return pte & nptAddress;
	}

	static constexpr PageStatus ptePageStatus(uint64_t pte) {
		if(ptePagePresent(pte))
			return 0;
		PageStatus status = page_status::present;
		if(pte & nptDirty)
			status |= page_status::dirty;
		return status;
	}

	static PageStatus pteClean(uint64_t *ptePtr) {
		auto pte = __atomic_fetch_and(ptePtr, ~nptDirty, __ATOMIC_RELAXED);
		return ptePageStatus(pte);
	}

	static constexpr uint64_t pteBuild(PhysicalAddr physical, PageFlags flags, CachingMode cachingMode) {
		auto pte = physical | nptPresent | nptUser;

		if(flags & page_access::write)
			pte |= nptWrite;
		if(!(flags & page_access::execute))
			pte |= nptXd;
		assert(cachingMode == CachingMode::null || cachingMode == CachingMode::writeBack);

		return pte;
	}


	static constexpr bool pteTablePresent(uint64_t pte) {
		return pte & nptPresent;
	}

	static constexpr PhysicalAddr pteTableAddress(uint64_t pte) {
		return pte & nptAddress;
	}

	static uint64_t pteNewTable() {
		auto newPtAddr = physicalAllocator->allocate(kPageSize);
		assert(newPtAddr != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor{newPtAddr};
		memset(accessor.get(), 0, kPageSize);

		return newPtAddr | nptPresent | nptUser | nptWrite;
	}
};

static_assert(CursorPolicy<NptCursorPolicy>);

using NptCursor = thor::PageCursor<NptCursorPolicy>;

NptPageSpace::NptPageSpace(PhysicalAddr root)
: PageSpace{root} { }

NptPageSpace::~NptPageSpace() {
	freePt<NptCursorPolicy, 3>(rootTable());
}

NptOperations::NptOperations(NptPageSpace *pageSpace)
: pageSpace_{pageSpace} { }

void NptOperations::retire(RetireNode *node) {
	// TODO: Shootdown needs to be implemented for NPT.
	(void)node;
}

bool NptOperations::submitShootdown(ShootNode *node) {
	// TODO: Shootdown needs to be implemented for NPT.
	(void)node;
	return false;
}

frg::expected<Error> NptOperations::mapPresentPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size, PageFlags flags) {
	return mapPresentPagesByCursor<NptCursor>(pageSpace_,
			va, view, offset, size, flags);
}

frg::expected<Error> NptOperations::remapPresentPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size, PageFlags flags) {
	return remapPresentPagesByCursor<NptCursor>(pageSpace_,
			va, view, offset, size, flags);
}

frg::expected<Error> NptOperations::faultPage(VirtualAddr va, MemoryView *view,
		uintptr_t offset, PageFlags flags) {
	return faultPageByCursor<NptCursor>(pageSpace_,
			va, view, offset, flags);
}

frg::expected<Error> NptOperations::cleanPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size) {
	return cleanPagesByCursor<NptCursor>(pageSpace_,
			va, view, offset, size);
}

frg::expected<Error> NptOperations::unmapPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size) {
	return unmapPagesByCursor<NptCursor>(pageSpace_,
			va, view, offset, size);
}

} // namespace thor::svm

using namespace thor;

enum {
	kPagePresent = (1 << 0),
	kPageWrite = (1 << 1),
	kPageUser = (1 << 2),
	kPageDirty = (1 << 6),
	kPageXd = (uint64_t{1} << 63),
	kPageAddress = uint64_t{0x000FFFFFFFFFF000}
};

Error thor::svm::NptSpace::map(uint64_t guestAddress, uint64_t hostAddress, int flags) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);

	PageAccessor spaceAccessor{spaceRoot};
	auto pml4e = reinterpret_cast<size_t *>(spaceAccessor.get());

	size_t pageFlags = kPagePresent | kPageUser;
	if(flags & page_access::write)
		pageFlags |= kPageWrite;
	if(!(flags & page_access::execute))
		pageFlags |= kPageXd;

	size_t *pdpte;
	if(!(pml4e[pml4eIdx] & kPagePresent)) {
		auto pdpte_ptr = physicalAllocator->allocate(kPageSize);
		if(reinterpret_cast<PhysicalAddr>(pdpte_ptr) == static_cast<PhysicalAddr>(-1))
			return Error::noMemory;

		pml4e[pml4eIdx] = (pdpte_ptr & kPageAddress) | kPagePresent | kPageUser | kPageWrite;
		PageAccessor pdpteAccessor{pdpte_ptr};
		memset(pdpteAccessor.get(), 0, kPageSize);
	}
	PageAccessor pdpteAccessor{pml4e[pml4eIdx] & kPageAddress};
	pdpte = reinterpret_cast<size_t *>(pdpteAccessor.get());

	size_t *pde;
	if(!(pdpte[pdpteIdx] & kPagePresent)) {
		auto pdpte_ptr = physicalAllocator->allocate(kPageSize);
		if(reinterpret_cast<PhysicalAddr>(pdpte_ptr) == static_cast<PhysicalAddr>(-1))
			return Error::noMemory;
		
		pdpte[pdpteIdx] = (pdpte_ptr & kPageAddress) | kPagePresent | kPageUser | kPageWrite;
		PageAccessor pdpteAccessor{pdpte_ptr};
		memset(pdpteAccessor.get(), 0, kPageSize);
	}
	PageAccessor pdeAccessor{pdpte[pdpteIdx] & kPageAddress};
	pde = reinterpret_cast<size_t *>(pdeAccessor.get());

	size_t *pte;
	if(!(pde[pdeIdx] & kPagePresent)) {
		auto pt_ptr = physicalAllocator->allocate(kPageSize);
		if(reinterpret_cast<PhysicalAddr>(pt_ptr) == static_cast<PhysicalAddr>(-1))
			return Error::noMemory;

		pde[pdeIdx] = (pt_ptr & kPageAddress) | kPagePresent | kPageUser | kPageWrite;
		PageAccessor pdpteAccessor{pt_ptr};
		memset(pdpteAccessor.get(), 0, kPageSize);
	}
	PageAccessor pteAccessor{pde[pdeIdx] & kPageAddress};
	pte = reinterpret_cast<size_t *>(pteAccessor.get());

	size_t entry = (hostAddress & kPageAddress) | pageFlags;
	pte[pteIdx] = entry;

	return Error::success;
}

bool thor::svm::NptSpace::isMapped(VirtualAddr guestAddress) {
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);

	PageAccessor spaceAccessor{spaceRoot};
	size_t *pml4e = reinterpret_cast<size_t *>(spaceAccessor.get());

	size_t *pdpte;
	if(!(pml4e[pml4eIdx] & kPagePresent)) {
		return false;
	} else {
		PageAccessor pdpteAccessor{pml4e[pml4eIdx] & kPageAddress};
		pdpte = reinterpret_cast<size_t *>(pdpteAccessor.get());
	}

	size_t *pde;
	if(!(pdpte[pdpteIdx] & kPagePresent)) {
		return false;
	} else {
		PageAccessor pdeAccessor{pdpte[pdpteIdx] & kPageAddress};
		pde = reinterpret_cast<size_t *>(pdeAccessor.get());
	}

	size_t *pte;
	if(!(pde[pdeIdx] & kPagePresent)) {
		return false;
	} else {
		PageAccessor pteAccessor{pde[pdeIdx] & kPageAddress};
		pte = reinterpret_cast<size_t *>(pteAccessor.get());
	}

	return ((pte[pteIdx] & kPagePresent));
}

uintptr_t thor::svm::NptSpace::translate(uintptr_t guestAddress) {
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);
	size_t offset = (size_t)guestAddress & 0xFFF;

	PageAccessor spaceAccessor{spaceRoot};
	size_t *pml4e = reinterpret_cast<size_t *>(spaceAccessor.get());

	size_t *pdpte;
	if(!(pml4e[pml4eIdx] & kPagePresent)) {
		return -1;
	} else {
		PageAccessor pdpteAccessor{pml4e[pml4eIdx] & kPageAddress};
		pdpte = reinterpret_cast<size_t *>(pdpteAccessor.get());
	}

	size_t *pde;
	if(!(pdpte[pdpteIdx] & kPagePresent)) {
		return -1;
	} else {
		PageAccessor pdeAccessor{pdpte[pdpteIdx] & kPageAddress};
		pde = reinterpret_cast<size_t *>(pdeAccessor.get());
	}

	size_t *pte;
	if(!(pde[pdeIdx] & kPagePresent)) {
		return -1;
	} else {
		PageAccessor pteAccessor{pde[pdeIdx] & kPageAddress};
		pte = reinterpret_cast<size_t *>(pteAccessor.get());
	}

	return (pte[pteIdx] & kPageAddress) + offset;
}

PageStatus thor::svm::NptSpace::unmap(uint64_t guestAddress) {
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);

	PageAccessor spaceAccessor{spaceRoot};
	size_t *pml4e = reinterpret_cast<size_t *>(spaceAccessor.get());

	size_t *pdpte;
	if(!(pml4e[pml4eIdx] & kPagePresent)) {
		return 0;
	} else {
		PageAccessor pdpteAccessor{pml4e[pml4eIdx] & kPageAddress};
		pdpte = reinterpret_cast<size_t *>(pdpteAccessor.get());
	}

	size_t *pde;
	if(!(pdpte[pdpteIdx] & kPagePresent)) {
		return 0;
	} else {
		PageAccessor pdeAccessor{pdpte[pdpteIdx] & kPageAddress};
		pde = reinterpret_cast<size_t *>(pdeAccessor.get());
	}

	size_t *pte;
	if(!(pde[pdeIdx] & kPagePresent)) {
		return 0;
	} else {
		PageAccessor pteAccessor{pde[pdeIdx] & kPageAddress};
		pte = reinterpret_cast<size_t *>(pteAccessor.get());
	}

	PageStatus status = page_status::present;
	if(pte[pteIdx] & kPageDirty)
		status |= page_status::dirty;

	pte[pteIdx] = 0;
	return status;
}

Error thor::svm::NptSpace::store(uintptr_t guestAddress, size_t size, const void *buffer) {
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

Error thor::svm::NptSpace::load(uintptr_t guestAddress, size_t size, void *buffer) {
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

bool thor::svm::NptSpace::submitShootdown(ShootNode *node) {
	infoLogger() << "thor: NptSpace::submitShootdown is a stub" << frg::endlog;

	node->complete();
	return false;
}

void thor::svm::NptSpace::retire(RetireNode *node) {
	infoLogger() << "thor: NptSpace::retire is a stub" << frg::endlog;
	
	node->complete();
}

thor::svm::NptSpace::~NptSpace() {
	PageAccessor spaceAccessor{spaceRoot};
	auto pml4e = reinterpret_cast<size_t *>(spaceAccessor.get());

	for(int i = 0; i < 512; i++) {
		if(pml4e[i] & kPagePresent) {
			PageAccessor pdpteAccessor{pml4e[i] & kPageAddress};
			auto pdpte = reinterpret_cast<size_t *>(pdpteAccessor.get());
			for(int j = 0; j < 512; j++) {
				if(pdpte[j] & kPagePresent) {
					PageAccessor pdeAccessor{pdpte[j] & kPageAddress};
					auto pde = reinterpret_cast<size_t *>(pdeAccessor.get());
					for(int k = 0; k < 512; k++) {
						if(pde[k] & kPagePresent) {
							physicalAllocator->free((size_t)(pde[k] & kPageAddress), kPageSize);
						}
					}
					physicalAllocator->free((size_t)(pdpte[j] & kPageAddress), kPageSize);
				}
			}
			physicalAllocator->free((size_t)(pml4e[i] & kPageAddress), kPageSize);
		}
	}
	physicalAllocator->free(spaceRoot, kPageSize);
}
