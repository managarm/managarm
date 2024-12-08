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
inline constexpr uint64_t kPageOuterSh = (2 << 8);
inline constexpr uint64_t kPageWb = (0 << 2);
inline constexpr uint64_t kPagenGnRnE = (2 << 2);
inline constexpr uint64_t kPagenGnRE = (3 << 2);
inline constexpr uint64_t kPageUc = (4 << 2);
inline constexpr uint64_t kPageAddress = 0xFFFFFFFFF000;

template <bool Kernel>
struct ARMCursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return 4; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & kPageValid;
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
		return ptePageStatus(pte);
	}

	static constexpr uint64_t pteBuild(PhysicalAddr physical, PageFlags flags, CachingMode cachingMode) {
		uint64_t pte = physical | kPageValid | kPageL3Page | kPageAccess;

		if constexpr(!Kernel) {
			pte |= kPageUser | kPageNotGlobal | kPageRO;
			if (flags & page_access::write)
				pte |= kPageShouldBeWritable;
		} else {
			if (!(flags & page_access::write))
				pte |= kPageRO;
		}
		if (!(flags & page_access::execute))
			pte |= kPageXN | kPagePXN;
		if (cachingMode == CachingMode::writeCombine)
			pte |= kPageUc | kPageOuterSh;
		else if (cachingMode == CachingMode::uncached)
			pte |= kPagenGnRnE | kPageOuterSh;
		else if (cachingMode == CachingMode::mmio)
			pte |= kPagenGnRE | kPageOuterSh;
		else if (cachingMode == CachingMode::mmioNonPosted)
			pte |= kPagenGnRnE | kPageOuterSh;
		else {
			assert(cachingMode == CachingMode::null || cachingMode == CachingMode::writeBack);
			pte |= kPageWb | kPageInnerSh;
		}

		return pte;
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
