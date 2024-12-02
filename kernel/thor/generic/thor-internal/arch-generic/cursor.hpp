#pragma once

#include <stddef.h>
#include <concepts>
#include <tuple>

#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/arch-generic/asid.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

template <typename T>
concept CursorPolicy = requires (uint64_t pte, uint64_t *ptePtr,
		PhysicalAddr pa, PageFlags flags, CachingMode cachingMode,
		PageSpace *space, void (*fn)(void *),void *ctxt) {
	// Maximum possible number of table levels.
	{ T::maxLevels } -> std::convertible_to<size_t>;
	// Amount of levels currently in use.
	{ T::numLevels() } -> std::convertible_to<size_t>;
	// How many bits of the address each level resolves.
	{ T::bitsPerLevel } -> std::convertible_to<size_t>;

	// Check whether the given PTE say the page is present.
	{ T::ptePagePresent(pte) } -> std::same_as<bool>;
	// Get the page address from the given PTE.
	{ T::ptePageAddress(pte) } -> std::same_as<PhysicalAddr>;
	// Get the status (present, dirty) from the given PTE.
	{ T::ptePageStatus(pte) } -> std::same_as<PageStatus>;
	// Clean the given PTE (remove the dirty status).
	{ T::pteClean(ptePtr) } -> std::same_as<PageStatus>;
	// Construct a new PTE from the given parameters.
	{ T::pteBuild(pa, flags, cachingMode) } -> std::same_as<uint64_t>;

	// Check whether the given PTE say the table is present.
	{ T::pteTablePresent(pte) } -> std::same_as<bool>;
	// Get the table address from the given PTE.
	{ T::pteTableAddress(pte) } -> std::same_as<PhysicalAddr>;
	// Allocate a new page table and construct a PTE for it.
	{ T::pteNewTable() } -> std::same_as<uint64_t>;
};

template <CursorPolicy Policy>
struct PageCursor {
	inline static constexpr uintptr_t levelMask = (uintptr_t{1} << Policy::bitsPerLevel) - 1;
	inline static constexpr size_t lastLevel = Policy::maxLevels - 1;

	PageCursor(PageSpace *space, uintptr_t va)
	: space_{space}, va_{}, initialLevel_{Policy::maxLevels - Policy::numLevels()} {
		accessors_[initialLevel_] = {space->rootTable()};
		moveTo(va);
	}

private:
	constexpr size_t levelShift(size_t level) {
		return Policy::bitsPerLevel * (Policy::maxLevels - 1 - level) + 12;
	}

	uint64_t *currentPtePtr_() {
		return reinterpret_cast<uint64_t *>(accessors_[lastLevel].get())
			+ ((va_ >> 12) & levelMask);
	}

	uint64_t readCurrentPte_() {
		return __atomic_load_n(currentPtePtr_(), __ATOMIC_RELAXED);
	}

	uint64_t exchangeCurrentPte_(uint64_t value) {
		return __atomic_exchange_n(currentPtePtr_(), value, __ATOMIC_RELAXED);
	}

public:
	uintptr_t virtualAddress() {
		return va_;
	}

	void moveTo(uintptr_t va) {
		for(size_t i = initialLevel_ + 1; i < Policy::maxLevels; i++) {
			if((va_ ^ va) & (levelMask << levelShift(i))) {
				for(size_t j = i; j < Policy::maxLevels; j++) {
					accessors_[j] = {};
				}
				break;
			}
		}

		va_ = va;
		reloadLevel_(lastLevel);
	}

	void advance4k() {
		moveTo(va_ + kPageSize);
	}

	bool findPresent(uintptr_t limit) {
		while(va_ < limit) {
			if(!accessors_[lastLevel]) {
				advance4k();
				continue;
			}

			auto ptEnt = readCurrentPte_();
			if(Policy::ptePagePresent(ptEnt))
				return true;

			advance4k();
		}

		return false;
	}

	bool findDirty(uintptr_t limit) {
		while(va_ < limit) {
			if(!accessors_[lastLevel]) {
				advance4k();
				continue;
			}

			auto ptEnt = readCurrentPte_();
			if(Policy::ptePagePresent(ptEnt) && (Policy::ptePageStatus(ptEnt) & page_status::dirty))
				return true;

			advance4k();
		}

		return false;
	}

	void map4k(PhysicalAddr pa, PageFlags flags, CachingMode cachingMode) {
		if(!accessors_[lastLevel])
			realizePts_();

		auto ptEnt = readCurrentPte_();
		assert(!Policy::ptePagePresent(ptEnt));

		ptEnt = Policy::pteBuild(pa, flags, cachingMode);

		__atomic_store_n(currentPtePtr_(), ptEnt, __ATOMIC_RELAXED);
	}

	PageStatus remap4k(PhysicalAddr pa, PageFlags flags, CachingMode cachingMode) {
		if(!accessors_[lastLevel])
			realizePts_();

		auto ptEnt = Policy::pteBuild(pa, flags, cachingMode);
		ptEnt = exchangeCurrentPte_(ptEnt);

		return Policy::ptePageStatus(ptEnt);
	}

	PageStatus clean4k() {
		if(!accessors_[lastLevel])
			return 0;

		return Policy::pteClean(currentPtePtr_());
	}

	std::tuple<PageStatus, PhysicalAddr> unmap4k() {
		if(!accessors_[lastLevel])
			return {0, 0};

		auto ptEnt = exchangeCurrentPte_(0);
		return {Policy::ptePageStatus(ptEnt), Policy::ptePageAddress(ptEnt)};
	}

private:
	bool doReloadLevel_(PageAccessor &subPt, PageAccessor &pt, size_t level) {
		auto ptPtr = reinterpret_cast<uint64_t *>(pt.get())
			+ ((va_ >> levelShift(level)) & levelMask);
		auto ptEnt = __atomic_load_n(ptPtr, __ATOMIC_ACQUIRE);

		if(!Policy::pteTablePresent(ptEnt))
			return false;

		auto subPtPtr = Policy::pteTableAddress(ptEnt);
		subPt = PageAccessor{subPtPtr};
		return true;
	}

	bool reloadLevel_(size_t level) {
		if(accessors_[level]) /*[[likely]]*/
			return true;
		assert(level != initialLevel_);
		if(!reloadLevel_(level - 1))
			return false;
		return doReloadLevel_(accessors_[level], accessors_[level - 1], level - 1);
	}


	void doRealizeLevel_(PageAccessor &subPt, PageAccessor &pt, size_t level) {
		auto ptPtr = reinterpret_cast<uint64_t *>(pt.get())
			+ ((va_ >> levelShift(level)) & levelMask);
		auto ptEnt = __atomic_load_n(ptPtr, __ATOMIC_ACQUIRE);

		if(Policy::pteTablePresent(ptEnt)) {
			auto subPtPtr = Policy::pteTableAddress(ptEnt);
			subPt = PageAccessor{subPtPtr};
			return;
		}

		ptEnt = Policy::pteNewTable();
		auto subPtPtr = Policy::pteTableAddress(ptEnt);
		subPt = PageAccessor{subPtPtr};

		__atomic_store_n(ptPtr, ptEnt, __ATOMIC_RELEASE);
	}

	void realizeLevel_(size_t level) {
		if(accessors_[level]) /*[[likely]]*/
			return;
		assert(level != initialLevel_);
		realizeLevel_(level - 1);
		return doRealizeLevel_(accessors_[level], accessors_[level - 1], level - 1);
	}

	void realizePts_() {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&space_->tableMutex());
		{
			realizeLevel_(lastLevel);
		}
	}

private:
	PageSpace *space_;
	uintptr_t va_;

	size_t initialLevel_;

	PageAccessor accessors_[Policy::maxLevels];
};

// Free page tables recursively. Only frees the page table pages, not the leaf pages.
template<CursorPolicy Policy, size_t N, bool LowerHalfOnly = false>
void freePt(PhysicalAddr tblPa) {
	PageAccessor accessor{tblPa};
	auto tblPtr = reinterpret_cast<uint64_t *>(accessor.get());
	for(int i = 0; i < (LowerHalfOnly ? 256 : 512); i++) { // TODO: Use bitsPerLevel.
		assert(!Policy::ptePagePresent(tblPtr[i]));
		if(!Policy::pteTablePresent(tblPtr[i]))
			continue;
		auto subTblPa = Policy::pteTableAddress(tblPtr[i]);
		if constexpr (N > 1) {
			freePt<Policy, N - 1>(subTblPa);
		} else {
			// Free last level page table.
			physicalAllocator->free(subTblPa, kPageSize);
		}
	}

	// Free higher level page table.
	physicalAllocator->free(tblPa, kPageSize);
}

} // namespace thor
