#include <thor-internal/arch/npt.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/physical.hpp>

namespace thor::svm {

constexpr uint64_t nptPresent = UINT64_C(1) << 0;
constexpr uint64_t nptWrite = UINT64_C(1) << 1;
constexpr uint64_t nptUser = UINT64_C(1) << 2;
constexpr uint64_t nptDirty = UINT64_C(1) << 6;
constexpr uint64_t nptPageSize = UINT64_C(1) << 7;
constexpr uint64_t nptXd = UINT64_C(1) << 63;
constexpr uint64_t nptAddress = 0x000F'FFFF'FFFF'F000;

struct NptCursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return 4; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & nptPresent;
	}

	static constexpr bool ptePageCanAccess(uint64_t pte, PageFlags flags) {
		if (!(pte & nptPresent))
			return false;

		if (flags & page_access::write && !(pte & nptWrite))
			return false;

		if (flags & page_access::execute && (pte & nptXd))
			return false;

		return true;
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

	// Guest memory is always mapped write-back; see pteBuild().
	static inline constexpr uint64_t ptePageCachingMask = 0;

	static uint64_t pteClean(uint64_t *ptePtr) {
		return __atomic_fetch_and(ptePtr, ~nptDirty, __ATOMIC_RELAXED);
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

	static std::pair<uint64_t, bool> pteAge(uint64_t *ptePtr, bool) {
		return {__atomic_load_n(ptePtr, __ATOMIC_RELAXED), false};
	}

	static constexpr void pteWriteBarrier(uint64_t *) { }
	static constexpr void pteSyncICache(uintptr_t) { }

	static constexpr bool pteIsHugePage(uint64_t pte) {
		return (pte & nptPresent) && (pte & nptPageSize);
	}

	static constexpr bool pteTablePresent(uint64_t pte) {
		return pte & nptPresent;
	}

	static constexpr PhysicalAddr pteTableAddress(uint64_t pte) {
		return pte & nptAddress;
	}

	static uint64_t pteNewTable(PhysicalAddr newPtAddr) {
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

// VMRUN flushes the TLB (see the TLB control of the VMCB).
// Only a CPU that is currently in the guest can have stale translations.
// TODO: With ASIDs, we need to invalidate like EptOperations does.
void NptOperations::retire(RetireNode *node) {
	node->complete();
}

bool NptOperations::submitShootdown(ShootNode *node) {
	// If the guest is currently running, we interrupt it and let the CPU running it
	// complete the shootdown.
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

frg::expected<Error, PagesAffected> NptOperations::mapPresentPages(VirtualAddr va, MemoryView *view,
		uintptr_t offset, size_t size, PageFlags flags, CachingMode mode, RevokeBatch &batch,
		PageMagazine &tables) {
	return mapPresentPagesByCursor<NptCursor>(pageSpace_,
			va, view, offset, size, flags, mode, batch, tables);
}

frg::expected<Error, PagesAffected> NptOperations::restrictPages(VirtualAddr va,
		size_t size, PageFlags flags, RevokeBatch &batch) {
	return restrictPagesByCursor<NptCursor>(pageSpace_, va, size, flags, batch);
}

frg::expected<Error, PagesAffected> NptOperations::faultPage(VirtualAddr va, MemoryView *view,
		uintptr_t offset, FetchFlags fetchFlags, PageFlags flags, CachingMode mode,
		RevokeBatch &batch, PageMagazine &tables) {
	return faultPageByCursor<NptCursor>(pageSpace_,
			va, view, offset, fetchFlags, flags, mode, batch, tables);
}

frg::expected<Error, PagesAffected> NptOperations::cleanPages(VirtualAddr va, size_t size,
		RevokeBatch &batch) {
	return cleanPagesByCursor<NptCursor>(pageSpace_, va, size, batch);
}

frg::expected<Error, PagesAffected> NptOperations::unmapPages(VirtualAddr va, size_t size,
		RevokeBatch &batch) {
	return unmapPagesByCursor<NptCursor>(pageSpace_, va, size, batch);
}

frg::expected<Error, PagesAffected> NptOperations::agePages(VirtualAddr, size_t, bool,
		RevokeBatch &) {
	return PagesAffected{};
}

NptSpace::NptSpace(CtorToken, PhysicalAddr root)
: VirtualizedPageSpace{&nptOps_}, nptOps_{&pageSpace_}, pageSpace_{root} { }

bool NptSpace::enterGuest(CpuData *cpu) {
	CpuData *expected = nullptr;
	return pageSpace_.guestCpu.compare_exchange_strong(expected, cpu, std::memory_order_seq_cst);
}

void NptSpace::leaveGuest() {
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

NptSpace::~NptSpace() { }

} // namespace thor::svm
