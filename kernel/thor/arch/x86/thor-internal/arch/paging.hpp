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

inline constexpr int pteAgeShift = 9;

constexpr uint64_t ptePresent = 0x1;
constexpr uint64_t pteWrite = 0x2;
constexpr uint64_t pteUser = 0x4;
constexpr uint64_t ptePwt = 0x8;
constexpr uint64_t ptePcd = 0x10;
constexpr uint64_t pteAccessed = 0x20;
constexpr uint64_t pteDirty = 0x40;
constexpr uint64_t ptePat = 0x80;
constexpr uint64_t pteGlobal = 0x100;
constexpr uint64_t pteAgeMask = UINT64_C(3) << pteAgeShift;
constexpr uint64_t pteXd = 0x8000000000000000;
constexpr uint64_t pteAddress = 0x000F'FFFF'FFFF'F000;

inline int getLowerHalfBits() {
	return 47;
}

template <bool Kernel>
struct X86CursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return 4; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & ptePresent;
	}

	static constexpr bool ptePageCanAccess(uint64_t pte, PageFlags flags) {
		if (!(pte & ptePresent))
			return false;

		if constexpr (!Kernel) {
			if (!(pte & pteUser))
				return false;
		}

		if (flags & page_access::execute && (pte & pteXd))
			return false;

		if (flags & page_access::write && !(pte & pteWrite))
			return false;

		return true;
	}

	static constexpr PhysicalAddr ptePageAddress(uint64_t pte) {
		return pte & pteAddress;
	}

	static constexpr PageStatus ptePageStatus(uint64_t pte) {
		if(!(pte & ptePresent))
			return 0;
		PageStatus status = page_status::present;
		if(pte & pteDirty)
			status |= page_status::dirty;
		return status;
	}

	static uint64_t pteClean(uint64_t *ptePtr) {
		return __atomic_fetch_and(ptePtr, ~pteDirty, __ATOMIC_RELAXED);
	}

	static constexpr uint64_t pteBuild(PhysicalAddr physical, PageFlags flags, CachingMode cachingMode) {
		auto pte = physical | ptePresent | pteAccessed;

		if constexpr (Kernel)
			pte |= pteGlobal;
		else
			pte |= pteUser;
		if(flags & page_access::write)
			pte |= pteWrite;
		if(!(flags & page_access::execute))
			pte |= pteXd;
		if(cachingMode == CachingMode::writeThrough) {
			pte |= ptePwt;
		}else if(cachingMode == CachingMode::writeCombine) {
			pte |= ptePat | ptePwt;
		}else if(cachingMode == CachingMode::uncached || cachingMode == CachingMode::mmio
				|| cachingMode == CachingMode::mmioNonPosted) {
			pte |= ptePcd;
		}else{
			assert(cachingMode == CachingMode::null || cachingMode == CachingMode::writeBack);
		}

		return pte;
	}

	static std::pair<uint64_t, bool> pteAge(uint64_t *ptePtr, bool vacate) {
		uint64_t oldPte = __atomic_load_n(ptePtr, __ATOMIC_RELAXED);
		while(true) {
			if(!(oldPte & ptePresent))
				return {oldPte, false};
			uint64_t newPte;
			bool unmapped = false;
			if(oldPte & pteAccessed) {
				newPte = oldPte & ~(pteAccessed | pteAgeMask);
			} else {
				uint64_t age = (oldPte & pteAgeMask) >> pteAgeShift;
				if(age < 3) {
					newPte = (oldPte & ~pteAgeMask) | ((age + 1) << pteAgeShift);
				} else {
					if (!vacate)
						return {oldPte, false};
					newPte = 0;
					unmapped = true;
				}
			}
			if(__atomic_compare_exchange_n(ptePtr, &oldPte, newPte, false,
					__ATOMIC_RELAXED, __ATOMIC_RELAXED))
				return {oldPte, unmapped};
		}
	}

	static constexpr void pteWriteBarrier() { }
	static constexpr void pteSyncICache(uintptr_t) { }


	static constexpr bool pteTablePresent(uint64_t pte) {
		return pte & ptePresent;
	}

	static constexpr PhysicalAddr pteTableAddress(uint64_t pte) {
		return pte & pteAddress;
	}

	static uint64_t pteNewTable() {
		auto newPtAddr = physicalAllocator->allocate(kPageSize);
		assert(newPtAddr != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor{newPtAddr};
		memset(accessor.get(), 0, kPageSize);

		return newPtAddr | ptePresent | pteWrite | pteUser;
	}
};

using KernelCursorPolicy = X86CursorPolicy<true>;
static_assert(CursorPolicy<KernelCursorPolicy>);

using ClientCursorPolicy = X86CursorPolicy<false>;
static_assert(CursorPolicy<ClientCursorPolicy>);


struct KernelPageSpace : PageSpace {
	using Cursor = thor::PageCursor<KernelCursorPolicy>;

	static void initialize();

	static KernelPageSpace &global();

	// TODO: This should be private.
	explicit KernelPageSpace(PhysicalAddr pml4_address);

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
