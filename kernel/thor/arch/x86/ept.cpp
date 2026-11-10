#include <thor-internal/arch/ept.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/physical.hpp>

namespace thor::vmx {

constexpr uint64_t eptRead = UINT64_C(1) << 0;
constexpr uint64_t eptWrite = UINT64_C(1) << 1;
constexpr uint64_t eptExecute = UINT64_C(1) << 2;
constexpr uint64_t eptCacheType = UINT64_C(7) << 3;
constexpr uint64_t eptCacheWb = UINT64_C(6) << 3;
constexpr uint64_t eptIgnorePat = UINT64_C(1) << 6;
constexpr uint64_t eptPageSize = UINT64_C(1) << 7;
constexpr uint64_t eptDirty = UINT64_C(1) << 9;
// TODO: Support user-executable permissions (needs VM-x bit).
// constexpr uint64_t eptUserExecute = UINT64_C(1) << 10;
constexpr uint64_t eptAddress = 0x000F'FFFF'FFFF'F000;

bool eptAccessedDirty = false;

uint64_t eptPointer(PhysicalAddr rootTable) {
	// Write-back paging structures, 4-level walks.
	uint64_t eptp = rootTable | 6 | (4 - 1) << 3;
	if(eptAccessedDirty)
		eptp |= 1 << 6;
	return eptp;
}

void inveptSingleContext(PhysicalAddr rootTable) {
	// The descriptor needs to contain the full EPT pointer, not just the root table.
	EptPtr ptr = {eptPointer(rootTable), 0};
	asm volatile(
		"invept (%0), %1;"
		:
		: "r"(&ptr), "r"((uint64_t)1) : "memory"
	);
}

struct EptCursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return 4; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & eptRead;
	}

	static constexpr bool ptePageCanAccess(uint64_t pte, PageFlags flags) {
		if(flags & page_access::read && !(pte & eptRead))
			return false;

		if (flags & page_access::write && !(pte & eptWrite))
			return false;

		if (flags & page_access::execute && !(pte & eptExecute))
			return false;

		return true;
	}

	static constexpr PhysicalAddr ptePageAddress(uint64_t pte) {
		return pte & eptAddress;
	}

	static PageStatus ptePageStatus(uint64_t pte) {
		if(!ptePagePresent(pte))
			return 0;
		PageStatus status = page_status::present;
		// Without dirty flags, we have to assume that writable pages were written.
		if(eptAccessedDirty ? (pte & eptDirty) : (pte & eptWrite))
			status |= page_status::dirty;
		return status;
	}

	static inline constexpr uint64_t ptePageCachingMask = eptCacheType;

	static uint64_t pteClean(uint64_t *ptePtr) {
		return __atomic_fetch_and(ptePtr, ~eptDirty, __ATOMIC_RELAXED);
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

	static std::pair<uint64_t, bool> pteAge(uint64_t *ptePtr, bool) {
		return {__atomic_load_n(ptePtr, __ATOMIC_RELAXED), false};
	}

	static constexpr void pteWriteBarrier(uint64_t *) { }
	static constexpr void pteSyncICache(uintptr_t) { }

	static constexpr bool pteIsHugePage(uint64_t pte) {
		return (pte & eptRead) && (pte & eptPageSize);
	}

	static constexpr bool pteTablePresent(uint64_t pte) {
		return pte & eptRead;
	}

	static constexpr PhysicalAddr pteTableAddress(uint64_t pte) {
		return pte & eptAddress;
	}

	static uint64_t pteNewTable(PhysicalAddr newPtAddr) {
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

// These only invalidate the local CPU; EptSpace::enterGuest() invalidates before VM entries
// if the generation changed.
void EptOperations::retire(RetireNode *node) {
	pageSpace_->generation.fetch_add(1, std::memory_order_seq_cst);
	inveptSingleContext(pageSpace_->rootTable());
	node->complete();
}

bool EptOperations::submitShootdown(ShootNode *node) {
	pageSpace_->generation.fetch_add(1, std::memory_order_seq_cst);
	inveptSingleContext(pageSpace_->rootTable());

	// If the guest is currently running, we interrupt it and let the CPU running it
	// complete the shootdown. Otherwise, we do nothing because the next guest entry
	// will invalidate the cached translations after the generation was bumped.
	CpuData *cpu;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&pageSpace_->shootMutex);

		cpu = pageSpace_->guestCpu.load(std::memory_order_seq_cst);
		if(cpu)
			pageSpace_->shootQueue.push_back(node);
	}
	if(cpu) {
		sendPingIpi(cpu);
	}else{
		node->complete();
	}
	return false;
}

frg::expected<Error, PagesAffected> EptOperations::mapPresentPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size, PageFlags flags, CachingMode mode, RevokeBatch &batch,
		PageMagazine &tables) {
	return mapPresentPagesByCursor<EptCursor>(pageSpace_,
			va, view, offset, size, flags, mode, batch, tables);
}

frg::expected<Error, PagesAffected> EptOperations::restrictPages(VirtualAddr va,
		size_t size, PageFlags flags, RevokeBatch &batch) {
	return restrictPagesByCursor<EptCursor>(pageSpace_, va, size, flags, batch);
}

frg::expected<Error, PagesAffected> EptOperations::faultPage(VirtualAddr va, MemoryView *view,
		uintptr_t offset, FetchFlags fetchFlags, PageFlags flags, CachingMode mode,
		RevokeBatch &batch, PageMagazine &tables) {
	return faultPageByCursor<EptCursor>(pageSpace_,
			va, view, offset, fetchFlags, flags, mode, batch, tables);
}

frg::expected<Error, PagesAffected> EptOperations::cleanPages(VirtualAddr va, size_t size,
		RevokeBatch &batch) {
	return cleanPagesByCursor<EptCursor>(pageSpace_, va, size, batch);
}

frg::expected<Error, PagesAffected> EptOperations::unmapPages(VirtualAddr va, size_t size,
		RevokeBatch &batch) {
	return unmapPagesByCursor<EptCursor>(pageSpace_, va, size, batch);
}

frg::expected<Error, PagesAffected> EptOperations::agePages(VirtualAddr, size_t, bool,
		RevokeBatch &) {
	return PagesAffected{};
}

EptSpace::EptSpace(CtorToken, PhysicalAddr root)
: VirtualizedPageSpace{&eptOps_}, invalidatedGenerations_{getCpuCount(), *kernelAlloc},
		eptOps_{&pageSpace_}, pageSpace_{root} { }

bool EptSpace::enterGuest(CpuData *cpu) {
	// This needs to happen before we read the generation, so that EptOperations::submitShootdown()
	// can kick us if we happen to return true from this function before the generation is bumped.
	CpuData *expected = nullptr;
	if(!pageSpace_.guestCpu.compare_exchange_strong(expected, cpu, std::memory_order_seq_cst))
		return false;

	// The root table of a destroyed EPT may be reused; new spaces start with 0.
	auto generation = pageSpace_.generation.load(std::memory_order_seq_cst) + 1;
	auto &invalidated = invalidatedGenerations_[cpu->cpuIndex];
	if(invalidated.load(std::memory_order_relaxed) != generation) {
		inveptSingleContext(rootTable());
		invalidated.store(generation, std::memory_order_relaxed);
	}
	return true;
}

void EptSpace::leaveGuest() {
	while(true) {
		ShootNode *node;
		{
			auto lock = frg::guard(&pageSpace_.shootMutex);

			pageSpace_.guestCpu.store(nullptr, std::memory_order_seq_cst);
			node = pageSpace_.shootQueue.front();
			if(node)
				pageSpace_.shootQueue.erase(node);
		}
		if(!node)
			break;
		node->complete();
	}
}

EptSpace::~EptSpace() { }

} // namespace thor
