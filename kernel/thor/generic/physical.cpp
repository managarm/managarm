#include <assert.h>
#include <initializer_list>
#include <frg/algorithm.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/physical.hpp>

namespace thor {

static bool logPhysicalAllocs = false;

THOR_DEFINE_ELF_NOTE(memoryLayoutNote){elf_note_type::memoryLayout, {}};
THOR_DEFINE_ELF_NOTE(physicalMemoryNote){elf_note_type::physicalMemory, {}};

frg::optional<CachingMode> determineFirmwareCachingMode(PhysicalAddr physical, size_t size) {
	auto *entries = reinterpret_cast<const EirFirmwareMemory *>(physicalMemoryNote->firmwareEntriesPtr);
	size_t numEntries = physicalMemoryNote->numFirmwareEntries;

	// Find the entry that contains the start of the range.
	const EirFirmwareMemory *foundEntry = nullptr;
	for(size_t i = 0; i < numEntries; i++) {
		if(entries[i].address <= physical && physical < entries[i].address + entries[i].size) {
			foundEntry = &entries[i];
			break;
		}
	}

	// Treat ranges outside of the firmware memory map as device memory.
	if(!foundEntry)
		return CachingMode::mmioNonPosted;
	auto &entry = *foundEntry;

	if(physical + size > entry.address + entry.size) {
		warningLogger() << "thor: Physical range 0x" << frg::hex_fmt{physical}
				<< " - 0x" << frg::hex_fmt{physical + size}
				<< " straddles multiple firmware memory map entries" << frg::endlog;
		return frg::null_opt;
	}

	if(entry.attributes & eir_memory_attrs::wb)
		return CachingMode::writeBack;
	// We do not have a CachingMode to represent WT, treat it like WC.
	if(entry.attributes & (eir_memory_attrs::wc | eir_memory_attrs::wt))
		return CachingMode::writeCombine;
	// If neither WB nor WC is supported, treat it as device memory.
	return CachingMode::mmioNonPosted;
}

void poisonPhysicalAccess(PhysicalAddr physical) {
	auto address = directPhysicalOffset() + physical;
	KernelPageSpace::global().unmapSingle4k(address);
	invalidatePage(globalBindingId, reinterpret_cast<void *>(address));
}

void poisonPhysicalWriteAccess(PhysicalAddr physical) {
	auto address = directPhysicalOffset() + physical;
	KernelPageSpace::global().unmapSingle4k(address);
	KernelPageSpace::global().mapSingle4k(address, physical, 0, CachingMode::null);
	invalidatePage(globalBindingId, reinterpret_cast<void *>(address));
}

// --------------------------------------------------------
// PhysicalChunkAllocator
// --------------------------------------------------------

PhysicalChunkAllocator::PhysicalChunkAllocator() {
}

void PhysicalChunkAllocator::bootstrapRegion(PhysicalAddr address,
		int order, size_t numRoots, int8_t *buddyTree) {
	if(_numRegions >= static_cast<int>(eirMaxMemoryRegions)) {
		infoLogger() << "thor: Ignoring memory region (can only handle "
				<< eirMaxMemoryRegions << " regions)" << frg::endlog;
		return;
	}
	assert(numRoots <= maxRootsPerRegion);

	int n = _numRegions++;
	auto &region = _allRegions[n];
	region.physicalBase = address;
	region.regionSize = numRoots << (order + kPageShift);
	region.buddyAccessor = BuddyAccessor{address, kPageShift,
			buddyTree, numRoots, order};

	// Eir already allocated from the buddy tree (e.g., for the kernel's page tables).
	size_t usedPages = 0;
	for(size_t i = 0; i < numRoots; i++) {
		auto freeItems = region.buddyAccessor.countFreeItems(i);
		region.usedPagesOfRoot[i] = region.pagesPerRoot() - freeItems;
		usedPages += region.usedPagesOfRoot[i];
	}

	auto currentTotal = _totalPages.load(std::memory_order_relaxed);
	auto currentUsed = _usedPages.load(std::memory_order_relaxed);
	auto currentFree = _freePages.load(std::memory_order_relaxed);
	_totalPages.store(currentTotal + (numRoots << order), std::memory_order_relaxed);
	_usedPages.store(currentUsed + usedPages, std::memory_order_relaxed);
	_freePages.store(currentFree + (numRoots << order) - usedPages, std::memory_order_relaxed);
}

void PhysicalChunkAllocator::assignRootClasses(size_t reserveBytes) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	assert(!_classesAssigned);

	size_t numFreeRoots = 0;
	for(int i = 0; i < _numRegions; i++) {
		auto &region = _allRegions[i];
		auto numRoots = region.buddyAccessor.numRoots();
		for(size_t r = 0; r < numRoots; r++) {
			auto cls = region.usedPagesOfRoot[r] ? RootClass::admitsPinned : RootClass::evictableOnly;
			region.rootClasses[r] = cls;
			if(cls == RootClass::evictableOnly)
				numFreeRoots++;
		}
	}

	// Reserve the lowest free roots. Eir does not sort the regions by address.
	int order[eirMaxMemoryRegions];
	for(int i = 0; i < _numRegions; i++)
		order[i] = i;
	frg::insertion_sort(order, order + _numRegions, [&] (int a, int b) {
		return _allRegions[a].physicalBase > _allRegions[b].physicalBase;
	});

	// Never reserve more than half of the free roots.
	auto maxReservedRoots = numFreeRoots / 2;
	size_t numReservedRoots = 0;
	size_t reservedBytes = 0;
	size_t largestReservedRoot = 0;
	for(int i = 0; i < _numRegions && reservedBytes < reserveBytes; i++) {
		auto &region = _allRegions[order[i]];
		auto numRoots = region.buddyAccessor.numRoots();
		auto rootBytes = region.pagesPerRoot() << kPageShift;
		for(size_t r = 0; r < numRoots && reservedBytes < reserveBytes; r++) {
			if(region.rootClasses[r] != RootClass::evictableOnly)
				continue;
			if(numReservedRoots >= maxReservedRoots)
				break;
			convertRoot_(region, r, RootClass::admitsContiguous);
			numReservedRoots++;
			reservedBytes += rootBytes;
			largestReservedRoot = frg::max(largestReservedRoot, rootBytes);
		}
	}
	if(reservedBytes < reserveBytes)
		warningLogger() << "thor: Reserved only " << (reservedBytes >> 20)
				<< " MiB of the requested " << (reserveBytes >> 20)
				<< " MiB for contiguous allocations" << frg::endlog;

	_classesAssigned = true;

	size_t classBytes[numRootClasses]{};
	for(int i = 0; i < _numRegions; i++) {
		auto &region = _allRegions[i];
		for(size_t r = 0; r < region.buddyAccessor.numRoots(); r++)
			classBytes[static_cast<size_t>(region.rootClasses[r])]
					+= region.pagesPerRoot() << kPageShift;
	}
	infoLogger() << "thor: Physical memory classes: "
			<< (classBytes[static_cast<size_t>(RootClass::evictableOnly)] >> 20)
			<< " MiB evictable only, "
			<< (classBytes[static_cast<size_t>(RootClass::admitsPinned)] >> 20)
			<< " MiB admit pinned, "
			<< (classBytes[static_cast<size_t>(RootClass::admitsContiguous)] >> 20)
			<< " MiB admit contiguous (largest reserved root: " << (largestReservedRoot >> 20)
			<< " MiB)" << frg::endlog;
}

PhysicalAddr PhysicalChunkAllocator::allocate(size_t size, int addressBits) {
	auto outcome = tryAllocate(size, PhysicalKind::pinned, addressBits);
	if(!outcome)
		return static_cast<PhysicalAddr>(-1);
	return *outcome;
}

std::expected<PhysicalAddr, Error> PhysicalChunkAllocator::tryAllocate(size_t size,
		PhysicalKind kind, int addressBits) {
	// TODO: This could be solved better.
	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;
	assert(size == (size_t(kPageSize) << target));

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	assert(_classesAssigned);

	if(logPhysicalAllocs)
		infoLogger() << "thor: Allocating physical memory of order "
					<< (target + kPageShift) << frg::endlog;

	frg::optional<Allocation> allocation;
	switch(kind) {
	case PhysicalKind::evictable:
		// If possible, keep holes in admitsPinned roots for pinned frames,
		// and use admitsContiguous roots only once everything else is full.
		for(auto cls : {RootClass::evictableOnly, RootClass::admitsPinned, RootClass::admitsContiguous}) {
			allocation = allocateInClass_(target, addressBits, cls);
			if(allocation)
				break;
		}
		break;
	case PhysicalKind::pinned:
		allocation = allocateInClass_(target, addressBits, RootClass::admitsPinned);
		if(!allocation)
			allocation = allocateByConverting_(target, addressBits,
					RootClass::admitsPinned);
		break;
	case PhysicalKind::contiguous:
		// Holes in admitsPinned roots cost nothing since these roots are never emptied anyway.
		// Converting an evictableOnly root shrinks the memory that can be emptied, so it comes last.
		allocation = allocateInClass_(target, addressBits, RootClass::admitsPinned);
		if(!allocation)
			allocation = allocateInClass_(target, addressBits,
					RootClass::admitsContiguous);
		if(!allocation)
			allocation = allocateByConverting_(target, addressBits,
					RootClass::admitsContiguous);
		break;
	}
	if(!allocation)
		return std::unexpected{Error::noMemory};

	accountAllocation_(*allocation, size / kPageSize);
	return allocation->physical;
}

template<typename F>
frg::optional<PhysicalChunkAllocator::Allocation>
PhysicalChunkAllocator::search_(int order, int addressBits, F rootFilter) {
	for(bool allowLow : {false, true}) {
		for(int i = 0; i < _numRegions; i++) {
			auto &region = _allRegions[i];
			// Note that Eir cuts regions in such a way that they never cross 4GiB.
			if(!allowLow && region.physicalBase < (PhysicalAddr{1} << 32))
				continue;
			if(order > region.buddyAccessor.tableOrder())
				continue;

			auto physical = region.buddyAccessor.allocate(order, addressBits,
					[&] (BuddyAccessor::AddressType rootIndex) {
						return rootFilter(region, rootIndex);
					});
			if(physical == BuddyAccessor::illegalAddress)
				continue;
			assert(!(physical % (size_t(kPageSize) << order)));
			return Allocation{&region, physical};
		}
	}
	return frg::null_opt;
}

frg::optional<PhysicalChunkAllocator::Allocation>
PhysicalChunkAllocator::allocateInClass_(int order, int addressBits, RootClass cls, bool requireFreeRoot) {
	return search_(order, addressBits, [&] (Region &region, size_t rootIndex) {
		if(region.rootClasses[rootIndex] != cls)
			return false;
		if(requireFreeRoot && region.buddyAccessor.rootFreeOrder(rootIndex)
				!= region.buddyAccessor.tableOrder())
			return false;
		return true;
	});
}

frg::optional<PhysicalChunkAllocator::Allocation>
PhysicalChunkAllocator::allocateByConverting_(int order, int addressBits, RootClass to) {
	// Prefer entirely free roots: converting a partially used root traps its evictable frames
	// in a root that can no longer be emptied.
	auto allocation = allocateInClass_(order, addressBits, RootClass::evictableOnly, true);
	if(!allocation)
		allocation = allocateInClass_(order, addressBits, RootClass::evictableOnly);
	if(!allocation)
		return frg::null_opt;

	auto &region = *allocation->region;
	convertRoot_(region, region.rootOfAddress(allocation->physical), to);
	return allocation;
}

void PhysicalChunkAllocator::convertRoot_(Region &region, size_t rootIndex, RootClass to) {
	region.rootClasses[rootIndex] = to;
}

void PhysicalChunkAllocator::accountAllocation_(Allocation allocation, size_t numPages) {
	auto &region = *allocation.region;
	auto rootIndex = region.rootOfAddress(allocation.physical);
	// Chunks are aligned to their size relative to the region, so they never straddle roots.
	assert(region.rootOfAddress(allocation.physical + (numPages << kPageShift) - 1) == rootIndex);
	region.usedPagesOfRoot[rootIndex] += numPages;

	auto currentFree = _freePages.load(std::memory_order_relaxed);
	auto currentUsed = _usedPages.load(std::memory_order_relaxed);
	assert(currentFree >= numPages);
	_freePages.store(currentFree - numPages, std::memory_order_relaxed);
	_usedPages.store(currentUsed + numPages, std::memory_order_relaxed);
}

void PhysicalChunkAllocator::free(PhysicalAddr address, size_t size) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	
	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;

	for(int i = 0; i < _numRegions; i++) {
		auto &region = _allRegions[i];
		if(address < region.physicalBase)
			continue;
		if(address + size - region.physicalBase > region.regionSize)
			continue;

		region.buddyAccessor.free(address, target);

		auto numPages = size / kPageSize;
		auto rootIndex = region.rootOfAddress(address);
		assert(region.usedPagesOfRoot[rootIndex] >= numPages);
		region.usedPagesOfRoot[rootIndex] -= numPages;

		auto currentFree = _freePages.load(std::memory_order_relaxed);
		auto currentUsed = _usedPages.load(std::memory_order_relaxed);
		assert(currentUsed >= numPages);
		_freePages.store(currentFree + numPages, std::memory_order_relaxed);
		_usedPages.store(currentUsed - numPages, std::memory_order_relaxed);
		return;
	}

	assert(!"Physical page is not part of any region");
}

PhysicalWindow::PhysicalWindow(PhysicalAddr physical, size_t size, CachingMode caching)
: size_{size} {
	uintptr_t lowAddr = physical & ~(kPageSize - 1);
	uintptr_t highAddr = ((physical + size) + (kPageSize - 1)) & ~(kPageSize - 1);
	size_t diff = highAddr - lowAddr;
	pages_ = diff / kPageSize;

	auto windowBase = KernelVirtualMemory::global().allocate(pages_ * kPageSize);
	for(size_t i = 0; i < pages_; i++) {
		KernelPageSpace::global().mapSingle4k(VirtualAddr(windowBase) + i * kPageSize,
				lowAddr + i * kPageSize, page_access::write, caching);
	}

	window_ = reinterpret_cast<void *>(uintptr_t(windowBase) + (physical - lowAddr));
}

PhysicalWindow::~PhysicalWindow() {
	auto windowBase = reinterpret_cast<uintptr_t>(window_) & ~(kPageSize - 1);

	for(size_t i = 0; i < pages_; i++) {
		KernelPageSpace::global().unmapSingle4k(VirtualAddr(windowBase) + i * kPageSize);
	}
}

} // namespace thor
