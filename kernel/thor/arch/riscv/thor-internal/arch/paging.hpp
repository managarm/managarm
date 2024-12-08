#pragma once

#include <assert.h>
#include <atomic>
#include <thor-internal/arch-generic/asid.hpp>
#include <thor-internal/arch-generic/cursor.hpp>
#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

constexpr uint64_t pteValid = UINT64_C(1) << 0;
constexpr uint64_t pteRead = UINT64_C(1) << 1;
constexpr uint64_t pteWrite = UINT64_C(1) << 2;
constexpr uint64_t pteExecute = UINT64_C(1) << 3;
constexpr uint64_t pteUser = UINT64_C(1) << 4;
constexpr uint64_t pteGlobal = UINT64_C(1) << 5;
constexpr uint64_t pteAccess = UINT64_C(1) << 6;
constexpr uint64_t pteDirty = UINT64_C(1) << 7;
constexpr uint64_t ptePpnMask = (((UINT64_C(1) << 44) - 1) << 10);

inline int getLowerHalfBits() { return 12 + 9 * riscvConfigNote->numPtLevels - 1; }

template <bool Kernel>
struct RiscvCursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return riscvConfigNote->numPtLevels; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return (pte & pteValid) && (pte & pteRead);
	}

	static constexpr PhysicalAddr ptePageAddress(uint64_t pte) { return (pte & ptePpnMask) << 2; }

	static constexpr PageStatus ptePageStatus(uint64_t pte) {
		if (!(pte & pteValid) || !(pte & pteRead))
			return 0;
		PageStatus status = page_status::present;
		if (pte & pteDirty)
			status |= page_status::dirty;
		return status;
	}

	static PageStatus pteClean(uint64_t *ptePtr) {
		auto pte = __atomic_fetch_and(ptePtr, ~pteDirty, __ATOMIC_RELAXED);
		return ptePageStatus(pte);
	}

	static constexpr uint64_t
	pteBuild(PhysicalAddr physical, PageFlags flags, CachingMode cachingMode) {
		auto pte = (physical >> 2) | pteValid | pteRead;

		if constexpr (Kernel)
			pte |= pteGlobal;
		else
			pte |= pteUser;
		if (flags & page_access::write)
			pte |= pteWrite;
		if (flags & page_access::execute)
			pte |= pteExecute;
		// TODO: Support caching modes.
		(void)cachingMode;

		return pte;
	}

	static constexpr bool pteTablePresent(uint64_t pte) { return pte & pteValid; }

	static constexpr PhysicalAddr pteTableAddress(uint64_t pte) { return (pte & ptePpnMask) << 2; }

	static uint64_t pteNewTable() {
		auto newPtAddr = physicalAllocator->allocate(kPageSize);
		assert(newPtAddr != PhysicalAddr(-1) && "OOM");

		PageAccessor accessor{newPtAddr};
		memset(accessor.get(), 0, kPageSize);

		return (newPtAddr >> 2) | pteValid;
	}
};

using KernelCursorPolicy = RiscvCursorPolicy<true>;
static_assert(CursorPolicy<KernelCursorPolicy>);

using ClientCursorPolicy = RiscvCursorPolicy<false>;
static_assert(CursorPolicy<ClientCursorPolicy>);

struct KernelPageSpace : PageSpace {
public:
	using Cursor = thor::PageCursor<KernelCursorPolicy>;

	static void initialize();

	static KernelPageSpace &global();

	// TODO: This should be private.
	explicit KernelPageSpace(PhysicalAddr satp);

	KernelPageSpace(const KernelPageSpace &) = delete;
	KernelPageSpace &operator=(const KernelPageSpace &) = delete;

	void mapSingle4k(
	    VirtualAddr pointer, PhysicalAddr physical, uint32_t flags, CachingMode caching_mode
	);
	PhysicalAddr unmapSingle4k(VirtualAddr pointer);
};

struct ClientPageSpace : PageSpace {
public:
	using Cursor = thor::PageCursor<ClientCursorPolicy>;

	ClientPageSpace();

	ClientPageSpace(const ClientPageSpace &) = delete;

	~ClientPageSpace();

	ClientPageSpace &operator=(const ClientPageSpace &) = delete;

	bool updatePageAccess(VirtualAddr pointer);
};

} // namespace thor
