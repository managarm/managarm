#pragma once

#include <atomic>

#include <assert.h>
#include <thor-internal/types.hpp>
#include <thor-internal/work-queue.hpp>
#include <thor-internal/physical.hpp>

#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/arch-generic/asid.hpp>
#include <thor-internal/arch-generic/cursor.hpp>

namespace thor {

inline constexpr uint64_t kPageValid = 1;
inline constexpr uint64_t kPageTable = (1 << 1);
inline constexpr uint64_t kPageL3Page = (1 << 1);
inline constexpr uint64_t kPageXN = (uint64_t(1) << 54);
inline constexpr uint64_t kPagePXN = (uint64_t(1) << 53);
inline constexpr uint64_t kPageShouldBeWritable = (uint64_t(1) << 55);
inline constexpr uint64_t kPageNotGlobal = (1 << 11);
inline constexpr uint64_t kPageAccess = (1 << 10);
inline constexpr uint64_t kPageRO = (1 << 7);
inline constexpr uint64_t kPageUser = (1 << 6);
inline constexpr uint64_t kPageInnerSh = (3 << 8);
inline constexpr uint64_t kPageWb = (0 << 2);
inline constexpr uint64_t kPagenGnRnE = (2 << 2);
inline constexpr uint64_t kPagenGnRE = (3 << 2);
inline constexpr uint64_t kPageUc = (4 << 2);
inline constexpr uint64_t kPageAddress = 0xFFFFFFFFF000;

inline int getLowerHalfBits() {
	return 48;
}


inline size_t icacheLineSize() {
	uint64_t ctr;
	asm ("mrs %0, ctr_el0" : "=r"(ctr));

	return (ctr & 0b1111) << 4;
}

inline size_t dcacheLineSize() {
	uint64_t ctr;
	asm ("mrs %0, ctr_el0" : "=r"(ctr));

	return ((ctr >> 16) & 0b1111) << 4;
}

inline size_t isICachePIPT() {
	uint64_t ctr;
	asm ("mrs %0, ctr_el0" : "=r"(ctr));

	return ((ctr >> 14) & 0b11) == 0b11;
}


template <bool Kernel>
struct ARMCursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return 4; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & kPageValid;
	}

	static constexpr bool ptePageCanAccess(uint64_t pte, PageFlags flags) {
		if (!(pte & kPageValid))
			return false;

		if constexpr (!Kernel) {
			if (!(pte & kPageUser))
				return false;

			if (flags & page_access::execute && (pte & kPagePXN))
				return false;
		} else {
			if (flags & page_access::execute && (pte & kPageXN))
				return false;
		}

		if (flags & page_access::write && !(pte & kPageShouldBeWritable))
			return false;

		return true;
	}

	static constexpr PhysicalAddr ptePageAddress(uint64_t pte) {
		return pte & kPageAddress;
	}

	static constexpr PageStatus ptePageStatus(uint64_t pte) {
		if (!(pte & kPageValid))
			return 0;

		PageStatus ps = page_status::present;
		if ((pte & kPageShouldBeWritable) && !(pte & kPageRO)) {
			ps |= page_status::dirty;
		}

		return ps;
	}

	static PageStatus pteClean(uint64_t *ptePtr) {
		uint64_t pte = __atomic_fetch_or(ptePtr, kPageRO, __ATOMIC_RELAXED);
		pteWriteBarrier();
		return ptePageStatus(pte);
	}

	static constexpr uint64_t pteBuild(PhysicalAddr physical, PageFlags flags, CachingMode cachingMode) {
		uint64_t pte = physical | kPageValid | kPageL3Page | kPageAccess | kPageInnerSh;

		if constexpr(!Kernel) {
			pte |= kPageUser | kPageNotGlobal | kPageRO | kPagePXN;
			if (flags & page_access::write)
				pte |= kPageShouldBeWritable;
		} else {
			if (!(flags & page_access::write))
				pte |= kPageRO;
		}
		if (!(flags & page_access::execute))
			pte |= kPageXN | kPagePXN;
		if (cachingMode == CachingMode::writeCombine)
			pte |= kPageUc;
		else if (cachingMode == CachingMode::uncached)
			pte |= kPagenGnRnE;
		else if (cachingMode == CachingMode::mmio)
			pte |= kPagenGnRE;
		else if (cachingMode == CachingMode::mmioNonPosted)
			pte |= kPagenGnRnE;
		else {
			assert(cachingMode == CachingMode::null || cachingMode == CachingMode::writeBack);
			pte |= kPageWb;
		}

		return pte;
	}

	static constexpr void pteWriteBarrier() {
		// TODO(qookie): Linux avoids the barrier for the innermost level user pages,
		// by letting them potentially (rarely) taking an extra no-op page fault.
		// Investigate whether it's worth doing this as well.
		asm volatile ("dsb ishst; isb" ::: "memory");
	}

	static constexpr void pteSyncICache(uintptr_t pa) {
		auto dsz = dcacheLineSize(), isz = icacheLineSize();

		PageAccessor accessor{pa};
		auto va = reinterpret_cast<uintptr_t>(accessor.get());

		for (auto addr = va & ~(dsz - 1); addr < va + kPageSize; addr += dsz) {
			asm volatile ("dc cvau, %0" :: "r"(addr) : "memory");
		}
		asm volatile ("dsb ish");

		if (isICachePIPT()) {
			for (auto addr = va & ~(isz - 1); addr < va + kPageSize; addr += isz) {
				asm volatile ("ic ivau, %0" :: "r"(addr) : "memory");
			}
		} else {
			asm volatile ("ic ialluis" ::: "memory");
		}
		asm volatile ("dsb ish; isb");
	}


	static constexpr bool pteTablePresent(uint64_t pte) {
		return pte & kPageValid;
	}

	static constexpr PhysicalAddr pteTableAddress(uint64_t pte) {
		return pte & kPageAddress;
	}

	static uint64_t pteNewTable() {
		auto newPtAddr = physicalAllocator->allocate(kPageSize);
		assert(newPtAddr != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor{newPtAddr};
		memset(accessor.get(), 0, kPageSize);

		return newPtAddr | kPageValid | kPageTable;
	}
};

using KernelCursorPolicy = ARMCursorPolicy<true>;
static_assert(CursorPolicy<KernelCursorPolicy>);

using ClientCursorPolicy = ARMCursorPolicy<false>;
static_assert(CursorPolicy<ClientCursorPolicy>);


struct KernelPageSpace : PageSpace {
	using Cursor = thor::PageCursor<KernelCursorPolicy>;

	static void initialize();

	static KernelPageSpace &global();

	// TODO(qookie): This should be private, but the ctor is invoked by frigg
	explicit KernelPageSpace(PhysicalAddr ttbr1);

	KernelPageSpace(const KernelPageSpace &) = delete;
	KernelPageSpace &operator= (const KernelPageSpace &) = delete;

	void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
			uint32_t flags, CachingMode cachingMode);
	PhysicalAddr unmapSingle4k(VirtualAddr pointer);
};

struct ClientPageSpace : PageSpace {
	using Cursor = thor::PageCursor<ClientCursorPolicy>;

	ClientPageSpace();

	ClientPageSpace(const ClientPageSpace &) = delete;

	~ClientPageSpace();

	ClientPageSpace &operator= (const ClientPageSpace &) = delete;

	bool updatePageAccess(VirtualAddr pointer, PageFlags);
};

} // namespace thor
