#include <thor-internal/arch/npt.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/physical.hpp>

namespace thor::svm {

constexpr uint64_t nptPresent = UINT64_C(1) << 0;
constexpr uint64_t nptWrite = UINT64_C(1) << 1;
constexpr uint64_t nptUser = UINT64_C(1) << 2;
constexpr uint64_t nptDirty = UINT64_C(1) << 6;
constexpr uint64_t nptXd = UINT64_C(1) << 6;
constexpr uint64_t nptAddress = 0x000F'FFFF'FFFF'F000;

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
		if(!ptePagePresent(pte))
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
		uintptr_t offset, size_t size, PageFlags flags, CachingMode) {
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

NptSpace::NptSpace(PhysicalAddr root)
: VirtualizedPageSpace{&nptOps_}, nptOps_{&pageSpace_}, pageSpace_{root} { }

NptSpace::~NptSpace() { }

} // namespace thor::svm
