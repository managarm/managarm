#pragma once

#include <atomic>

#include <frg/list.hpp>
#include <assert.h>
#include <smarter.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/work-queue.hpp>
#include <thor-internal/physical.hpp>

#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/arch-generic/asid.hpp>
#include <thor-internal/arch-generic/cursor.hpp>

namespace thor {

// Functions for debugging kernel page access:
// Deny all access to the physical mapping.
void poisonPhysicalAccess(PhysicalAddr physical);
// Deny write access to the physical mapping.
void poisonPhysicalWriteAccess(PhysicalAddr physical);

struct KernelPageSpace : PageSpace {
	static void initialize();

	static KernelPageSpace &global();

	// TODO: This should be private.
	explicit KernelPageSpace(PhysicalAddr pml4_address);

	KernelPageSpace(const KernelPageSpace &) = delete;

	KernelPageSpace &operator= (const KernelPageSpace &) = delete;


	void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
			uint32_t flags, CachingMode caching_mode);
	PhysicalAddr unmapSingle4k(VirtualAddr pointer);

private:
	frg::ticket_spinlock _mutex;
};

constexpr uint64_t ptePresent = 0x1;
constexpr uint64_t pteWrite = 0x2;
constexpr uint64_t pteUser = 0x4;
constexpr uint64_t ptePwt = 0x8;
constexpr uint64_t ptePcd = 0x10;
constexpr uint64_t pteDirty = 0x40;
constexpr uint64_t ptePat = 0x80;
constexpr uint64_t pteGlobal = 0x100;
constexpr uint64_t pteXd = 0x8000000000000000;
constexpr uint64_t pteAddress = 0x000FFFFFFFFFF00;

struct ClientCursorPolicy {
	static inline constexpr size_t maxLevels = 4;
	static inline constexpr size_t bitsPerLevel = 9;

	static constexpr size_t numLevels() { return 4; }

	static constexpr bool ptePagePresent(uint64_t pte) {
		return pte & ptePresent;
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

	static PageStatus pteClean(uint64_t *ptePtr) {
		auto pte = __atomic_fetch_and(ptePtr, ~pteDirty, __ATOMIC_RELAXED);
		return ptePageStatus(pte);
	}

	static constexpr uint64_t pteBuild(PhysicalAddr physical, PageFlags flags, CachingMode cachingMode) {
		auto pte = physical | ptePresent | pteUser;
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
static_assert(CursorPolicy<ClientCursorPolicy>);


struct ClientPageSpace : PageSpace {
	using Cursor = thor::PageCursor<ClientCursorPolicy>;

	ClientPageSpace();

	ClientPageSpace(const ClientPageSpace &) = delete;

	~ClientPageSpace();

	ClientPageSpace &operator= (const ClientPageSpace &) = delete;

	bool updatePageAccess(VirtualAddr pointer);

private:
	frg::ticket_spinlock _mutex;
};

} // namespace thor
