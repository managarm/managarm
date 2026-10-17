#pragma once

#include <atomic>
#include <expected>

#include <eir/interface.hpp>
#include <frg/optional.hpp>
#include <frg/spinlock.hpp>
#include <frg/manual_box.hpp>
#include <physical-buddy.hpp>
#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/elf-notes.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/types.hpp>

namespace thor {

extern ManagarmElfNote<MemoryLayout> memoryLayoutNote;
extern ManagarmElfNote<PhysicalMemory> physicalMemoryNote;

inline uintptr_t directPhysicalOffset() {
	return memoryLayoutNote->directPhysical;
}

inline void *mapDirectPhysical(PhysicalAddr physical) {
	assert(physical < 0x4000'0000'0000);
	return reinterpret_cast<void *>(directPhysicalOffset() + physical);
}

inline PhysicalAddr reverseDirectPhysical(void *pointer) {
	return reinterpret_cast<uintptr_t>(pointer) - directPhysicalOffset();
}

// Determines the caching mode with which the given physical range may be mapped,
// based on the firmware memory map. Ranges that the firmware memory map does not
// describe are assumed to be device memory. Returns null if the range straddles
// multiple entries (mixing distinct memory types in one mapping is not meaningful).
frg::optional<CachingMode> determineFirmwareCachingMode(PhysicalAddr physical, size_t size);

struct PageAccessor {
	friend void swap(PageAccessor &a, PageAccessor &b) {
		using std::swap;
		swap(a._pointer, b._pointer);
	}

	PageAccessor()
	: _pointer{nullptr} { }

	PageAccessor(PhysicalAddr physical) {
		assert(physical != PhysicalAddr(-1) && "trying to access invalid physical page");
		assert(!(physical & 0xFFF) && "physical page is not aligned");
		assert(physical < 0x4000'0000'0000);
		_pointer = reinterpret_cast<void *>(directPhysicalOffset() + physical);
	}

	PageAccessor(const PageAccessor &) = delete;

	PageAccessor(PageAccessor &&other)
	: PageAccessor{} {
		swap(*this, other);
	}

	~PageAccessor() { }

	PageAccessor &operator= (PageAccessor other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return _pointer;
	}

	void *get() {
		return _pointer;
	}

private:
	void *_pointer;
};

struct PhysicalWindow {
	PhysicalWindow() = default;
	PhysicalWindow(PhysicalAddr physical, size_t size, CachingMode caching = CachingMode::null);
	~PhysicalWindow();

	PhysicalWindow(const PhysicalWindow &) = delete;

	PhysicalWindow(PhysicalWindow &&other)
	: PhysicalWindow() {
		swap(*this, other);
	}

	friend void swap(PhysicalWindow &x, PhysicalWindow &y) {
		using std::swap;
		swap(x.window_, y.window_);
		swap(x.pages_, y.pages_);
		swap(x.size_, y.size_);
	}

	PhysicalWindow &operator= (PhysicalWindow other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return window_;
	}

	void *get() {
		return window_;
	}

private:
	void *window_ = nullptr;
	size_t pages_ = 0;
	size_t size_ = 0;
};

// Functions for debugging kernel page access:
// Deny all access to the physical mapping.
void poisonPhysicalAccess(PhysicalAddr physical);
// Deny write access to the physical mapping.
void poisonPhysicalWriteAccess(PhysicalAddr physical);


// What an allocation is used for. Decides which buddy roots may serve it.
enum class PhysicalKind {
	// Frames that can be evicted and moved to different physical pages (e.g., ManagedSpace pages).
	evictable,
	// Frames that are never evicted or moved (e.g., kernel data structures).
	pinned,
	// Frames of multi-page contiguous allocations. Like pinned, never evicted or moved.
	contiguous,
};

// Class of a buddy root, i.e., the PhysicalKinds that it admits.
// Roots can be converted from evictableOnly to a less restricted class.
// However, we forbid transitions between less restricted classes;
// for example, admitsContiguous -> admitsPinned is forbidden
// to make it easier to satisfy contiguous allocations.
enum class RootClass : uint8_t {
	// Can only satisfy evictable allocation.
	evictableOnly,
	// Can satisfy evictable and contiguous allocations.
	admitsContiguous,
	// Can satisfy evictable, contiguous and pinned allocations.
	admitsPinned,
};

class PhysicalChunkAllocator {
	typedef frg::ticket_spinlock Mutex;
public:
	// Guaranteed by BuddyAccessor::suitableOrder().
	static constexpr size_t maxRootsPerRegion = 64;
	static constexpr size_t numRootClasses = 3;

	PhysicalChunkAllocator();
	
	void bootstrapRegion(PhysicalAddr address,
			int order, size_t numRoots, int8_t *buddyTree);
	// Assigns the initial class of each root: roots that Eir allocated from admit pinned frames,
	// the lowest free roots that cover reserveBytes admit contiguous frames, all others are
	// evictableOnly.
	// Must be called after bootstrapRegion() and before the first allocation.
	void assignRootClasses(size_t reserveBytes);

	// Allocates a naturally aligned power-of-two sized chunk.
	std::expected<PhysicalAddr, Error> tryAllocate(size_t size, PhysicalKind kind,
			int addressBits = 64);

	// Like tryAllocate() for PhysicalKind::pinned but returns -1 on failure.
	PhysicalAddr allocate(size_t size, int addressBits = 64);

	// kind must match the kind that the chunk was allocated with.
	void free(PhysicalAddr address, size_t size, PhysicalKind kind);

	// Like free() for PhysicalKind::pinned.
	void free(PhysicalAddr address, size_t size) {
		free(address, size, PhysicalKind::pinned);
	}

	size_t numTotalPages() {
		return _totalPages.load(std::memory_order_relaxed);
	}
	size_t numUsedPages() {
		return _usedPages.load(std::memory_order_relaxed);
	}
	size_t numFreePages() {
		return _freePages.load(std::memory_order_relaxed);
	}

private:
	// Callers must hold _mutex.
	PhysicalAddr allocateChunk_(size_t size, PhysicalKind kind, int addressBits);

	// Callers must hold _mutex.
	void freeChunk_(PhysicalAddr address, size_t size, PhysicalKind kind);

	struct Region {
		PhysicalAddr physicalBase;
		PhysicalAddr regionSize;
		BuddyAccessor buddyAccessor;
		// Protected by _mutex.
		RootClass rootClasses[maxRootsPerRegion];
		// Protected by _mutex.
		uint32_t usedPagesOfRoot[maxRootsPerRegion];
		// Pages of PhysicalKind::pinned and PhysicalKind::contiguous. Protected by _mutex.
		uint32_t unevictablePagesOfRoot[maxRootsPerRegion];

		size_t pagesPerRoot() {
			return size_t{1} << buddyAccessor.tableOrder();
		}
		size_t rootOfAddress(PhysicalAddr address) {
			return (address - physicalBase) >> (buddyAccessor.tableOrder() + kPageShift);
		}
	};

	struct Allocation {
		Region *region;
		PhysicalAddr physical;
	};

	// Allocates from the roots that satisfy rootFilter(region, rootIndex).
	// Prefers regions above 4 GiB so that low memory stays available for
	// allocations that are constrained to a limited address width (e.g., 32-bit DMA).
	// Callers must hold _mutex.
	template<typename F>
	frg::optional<Allocation> search_(int order, int addressBits, F rootFilter);

	// Allocate from roots of a specific class.
	// Callers must hold _mutex.
	frg::optional<Allocation> allocateInClass_(int order, int addressBits, RootClass cls,
			bool requireFreeRoot = false);

	// Allocates from an evictableOnly root and converts that root to the given class.
	// Callers must hold _mutex.
	frg::optional<Allocation> allocateByConverting_(int order, int addressBits, RootClass to);

	// Callers must hold _mutex.
	void convertRoot_(Region &region, size_t rootIndex, RootClass to);

	// Callers must hold _mutex.
	void accountAllocation_(Allocation allocation, size_t numPages, PhysicalKind kind);

	Mutex _mutex;

	Region _allRegions[eirMaxMemoryRegions];
	int _numRegions = 0;
	bool _classesAssigned = false;

	std::atomic<size_t> _totalPages{0};
	std::atomic<size_t> _usedPages{0};
	std::atomic<size_t> _freePages{0};
};

extern constinit frg::manual_box<PhysicalChunkAllocator> physicalAllocator;

} // namespace thor
