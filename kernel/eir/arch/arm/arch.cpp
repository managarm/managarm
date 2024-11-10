#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/debug.hpp>
#include <assert.h>

namespace eir {

uintptr_t eirTTBR[2];

void setupPaging() {
	eirTTBR[0] = allocPage();
	eirTTBR[1] = allocPage();

	for (size_t i = 0; i < 512; i++) {
		((uint64_t *)eirTTBR[0])[i] = 0;
		((uint64_t *)eirTTBR[1])[i] = 0;
	}
}

static inline constexpr uint64_t kPageValid = 1;
static inline constexpr uint64_t kPageTable = (1 << 1);
static inline constexpr uint64_t kPageL3Page = (1 << 1);
static inline constexpr uint64_t kPageXN = (uint64_t(1) << 54);
static inline constexpr uint64_t kPagePXN = (uint64_t(1) << 53);
static inline constexpr uint64_t kPageNotGlobal = (1 << 11);
static inline constexpr uint64_t kPageAccess = (1 << 10);
static inline constexpr uint64_t kPageRO = (1 << 7);
static inline constexpr uint64_t kPageInnerSh = (3 << 8);
static inline constexpr uint64_t kPageOuterSh = (2 << 8);
static inline constexpr uint64_t kPageWb = (0 << 2);
static inline constexpr uint64_t kPageGRE = (1 << 2);
static inline constexpr uint64_t kPagenGnRnE = (2 << 2);

void mapSingle4kPage(address_t address, address_t physical, uint32_t flags,
		CachingMode caching_mode) {
	auto ttbr = (address >> 63) & 1;
	auto l0 = (address >> 39) & 0x1FF;
	auto l1 = (address >> 30) & 0x1FF;
	auto l2 = (address >> 21) & 0x1FF;
	auto l3 = (address >> 12) & 0x1FF;

	auto l0_ent = ((uint64_t *)eirTTBR[ttbr])[l0];
	auto l1_ptr = l0_ent & 0xFFFFFFFFF000;
	if (!(l0_ent & kPageValid)) {
		uint64_t addr = allocPage();

		for(int i = 0; i < 512; i++)
			((uint64_t *)addr)[i] = 0;

		((uint64_t *)eirTTBR[ttbr])[l0] =
			addr | kPageValid | kPageTable;

		l1_ptr = addr;
	}

	auto l1_ent = ((uint64_t *)l1_ptr)[l1];
	auto l2_ptr = l1_ent & 0xFFFFFFFFF000;
	if (!(l1_ent & kPageValid)) {
		uint64_t addr = allocPage();

		for(int i = 0; i < 512; i++)
			((uint64_t *)addr)[i] = 0;

		((uint64_t *)l1_ptr)[l1] =
			addr | kPageValid | kPageTable;

		l2_ptr = addr;
	}

	auto l2_ent = ((uint64_t *)l2_ptr)[l2];
	auto l3_ptr = l2_ent & 0xFFFFFFFFF000;
	if (!(l2_ent & kPageValid)) {
		uint64_t addr = allocPage();

		for(int i = 0; i < 512; i++)
			((uint64_t *)addr)[i] = 0;

		((uint64_t *)l2_ptr)[l2] =
			addr | kPageValid | kPageTable;

		l3_ptr = addr;
	}

	auto l3_ent = ((uint64_t *)l3_ptr)[l3];

	if (l3_ent & kPageValid)
		eir::panicLogger() << "eir: Trying to map 0x" << frg::hex_fmt{address}
				<< " twice!" << frg::endlog;

	uint64_t new_entry = physical | kPageValid | kPageL3Page | kPageAccess;

	if (!(flags & PageFlags::write))
		new_entry |= kPageRO;
	if (!(flags & PageFlags::execute))
		new_entry |= kPageXN | kPagePXN;
	if (!(flags & PageFlags::global))
		new_entry |= kPageNotGlobal;

	if (caching_mode == CachingMode::writeCombine) {
		new_entry |= kPageGRE | kPageOuterSh;
	} else if (caching_mode == CachingMode::mmio) {
		new_entry |= kPagenGnRnE | kPageOuterSh;
	} else {
		assert(caching_mode == CachingMode::null);
		new_entry |= kPageWb | kPageInnerSh;
	}

	if (new_entry & (0b111ULL << 48)) {
		eir::infoLogger() << "Oops, reserved bits set when mapping 0x"
			<< frg::hex_fmt{physical} << " to 0x" << frg::hex_fmt{address}
			<< frg::endlog;

		eir::panicLogger() << "New entry value: 0x" << frg::hex_fmt{new_entry} << frg::endlog;
	}

	((uint64_t *)l3_ptr)[l3] = new_entry;
}

address_t getSingle4kPage(address_t address) {
	auto ttbr = (address >> 63) & 1;
	auto l0 = (address >> 39) & 0x1FF;
	auto l1 = (address >> 30) & 0x1FF;
	auto l2 = (address >> 21) & 0x1FF;
	auto l3 = (address >> 12) & 0x1FF;

	auto l0_ent = ((uint64_t *)eirTTBR[ttbr])[l0];
	auto l1_ptr = l0_ent & 0xFFFFFFFFF000;
	if (!(l0_ent & kPageValid))
		return -1;

	auto l1_ent = ((uint64_t *)l1_ptr)[l1];
	auto l2_ptr = l1_ent & 0xFFFFFFFFF000;
	if (!(l1_ent & kPageValid))
		return -1;

	auto l2_ent = ((uint64_t *)l2_ptr)[l2];
	auto l3_ptr = l2_ent & 0xFFFFFFFFF000;
	if (!(l2_ent & kPageValid))
		return -1;

	auto l3_ent = ((uint64_t *)l3_ptr)[l3];
	auto page_ptr = l3_ent & 0xFFFFFFFFF000;
	if (!(l3_ent & kPageValid))
		return -1;

	return page_ptr;
}

void initProcessorEarly() {
	eir::infoLogger() << "Starting Eir" << frg::endlog;

	uint64_t aa64mmfr0;
	asm volatile ("mrs %0, id_aa64mmfr0_el1" : "=r" (aa64mmfr0));

	if (aa64mmfr0 & (0xF << 28))
		eir::panicLogger() << "PANIC! This CPU doesn't support 4K memory translation granules" << frg::endlog;

	if ((aa64mmfr0 & 0xF) < 1)
		eir::panicLogger() << "PANIC! This CPU doesn't support at least 48 bit physical addresses (max "
			<< (aa64mmfr0 & 0xF) << ")" << frg::endlog;

	auto pa = frg::min(uint64_t(5), aa64mmfr0 & 0xF);

	uint64_t mair =
		0b11111111 | // Normal, Write-back RW-Allocate non-transient
		(0b00001100 << 8) | // Device, GRE
		(0b00000000 << 16)| // Device, nGnRnE
		(0b00000100 << 24)| // Device, nGnRE
		(0b01000100UL << 32); // Normal Non-cacheable

	asm volatile ("msr mair_el1, %0" :: "r" (mair));

	uint64_t tcr =
		(16 << 0) | // T0SZ=16
		(16 << 16) | // T1SZ=16
		(1 << 8) | // TTBR0 Inner WB RW-Allocate
		(1 << 10) | // TTBR0 Outer WB RW-Allocate
		(1 << 24) | // TTBR1 Inner WB RW-Allocate
		(1 << 26) | // TTBR1 Outer WB RW-Allocate
		(2 << 12) | // TTBR0 Inner shareable
		(2 << 28) | // TTBR1 Inner shareable
		(uint64_t(pa) << 32) | // 48-bit intermediate address
		(uint64_t(2) << 30); // TTBR1 4K granule

	asm volatile ("msr tcr_el1, %0" :: "r" (tcr));
}

// Returns Core region index
void initProcessorPaging(void *kernel_start, uint64_t &kernel_entry) {
	setupPaging();
	eir::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10) << " KiB"
			" after setting up paging" << frg::endlog;

	// Identically map the first 128 MiB so that we can activate paging
	// without causing a page fault.
	auto floor = reinterpret_cast<address_t>(&eirImageFloor) & ~address_t{0xFFF};
	auto ceiling = (reinterpret_cast<address_t>(&eirImageCeiling) + 0xFFF) & ~address_t{0xFFF};
	for(address_t addr = floor; addr < ceiling; addr += 0x1000)
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

	mapKasanShadow(0xFFFF'E000'0000'0000, 0x8000'0000);
}

} // namespace eir
