#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/debug.hpp>

namespace eir {

void setupPaging() {}

void mapSingle4kPage(address_t address, address_t physical, uint32_t flags,
		CachingMode caching_mode) {}

address_t getSingle4kPage(address_t address) { return -1; }

void initProcessorEarly() {
	eir::infoLogger() << "Starting Eir" << frg::endlog;
}

// Returns Core region index
void initProcessorPaging(void *kernel_start, uint64_t &kernel_entry) {
	setupPaging();
	eir::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10) << " KiB"
			" after setting up paging" << frg::endlog;

	// Identically map the first 128 MiB so that we can activate paging
	// without causing a page fault.
	for(address_t addr = 0; addr < 0x8000000; addr += 0x1000)
		mapSingle4kPage(addr, addr, PageFlags::write | PageFlags::execute);

	mapRegionsAndStructs();
#ifdef KERNEL_LOG_ALLOCATIONS
	allocLogRingBuffer();
#endif

	// Setup the kernel image.
	kernel_entry = loadKernelImage(kernel_start);
	eir::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10) << " KiB"
			" after loading the kernel" << frg::endlog;

	// Setup the kernel stack.
	for(address_t page = 0; page < 0x10000; page += pageSize)
		mapSingle4kPage(0xFFFF'FE80'0000'0000 + page, allocPage(), PageFlags::write);
	mapKasanShadow(0xFFFF'FE80'0000'0000, 0x10000);
	unpoisonKasanShadow(0xFFFF'FE80'0000'0000, 0x10000);

	mapKasanShadow(0xFFFF'E000'0000'0000, 0x4000'0000);
}

} // namespace eir
