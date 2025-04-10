#pragma once

#include <atomic>

#include <eir/interface.hpp>
#include <frg/spinlock.hpp>
#include <frg/manual_box.hpp>
#include <physical-buddy.hpp>
#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/elf-notes.hpp>
#include <thor-internal/types.hpp>

namespace thor {

extern ManagarmElfNote<MemoryLayout> memoryLayoutNote;

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


class PhysicalChunkAllocator {
	typedef frg::ticket_spinlock Mutex;
public:
	PhysicalChunkAllocator();
	
	void bootstrapRegion(PhysicalAddr address,
			int order, size_t numRoots, int8_t *buddyTree);

	PhysicalAddr allocate(size_t size, int addressBits = 64);
	void free(PhysicalAddr address, size_t size);

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
	Mutex _mutex;

	struct Region {
		PhysicalAddr physicalBase;
		PhysicalAddr regionSize;
		BuddyAccessor buddyAccessor;
	};

	Region _allRegions[8];
	int _numRegions = 0;

	std::atomic<size_t> _totalPages{0};
	std::atomic<size_t> _usedPages{0};
	std::atomic<size_t> _freePages{0};
};

extern constinit frg::manual_box<PhysicalChunkAllocator> physicalAllocator;

} // namespace thor
