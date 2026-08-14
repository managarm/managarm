#include <assert.h>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>

extern "C" void eirExcVectors();

extern "C" [[noreturn]] void eirEnterKernel(uint64_t entry, uint64_t stack);

extern "C" void
eirFlushDisableMmuEl2(uint64_t flushStart, uint64_t flushEnd, uint64_t dcLineSize, uint64_t sctlr);
extern "C" void
eirFlushDisableMmuEl1(uint64_t flushStart, uint64_t flushEnd, uint64_t dcLineSize, uint64_t sctlr);

extern "C" void eirEnterE2h(uint64_t hcr);

namespace eir {

namespace {

void disableMmu() {
	uint64_t currentel;
	asm volatile("mrs %0, currentel" : "=r"(currentel));

	uint64_t ctr;
	asm("mrs %0, ctr_el0" : "=r"(ctr));
	auto dcLineSize = 4 << ((ctr >> 16) & 0b1111);

	const auto &bootCaps = BootCaps::get();
	auto flushStart = bootCaps.imageStart & ~(dcLineSize - 1);
	auto flushEnd = (bootCaps.imageEnd + (dcLineSize - 1)) & ~(dcLineSize - 1);

	uint64_t sctlr;
	if ((currentel >> 2) == 2) {
		asm volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
		eirFlushDisableMmuEl2(flushStart, flushEnd, dcLineSize, sctlr);
	} else {
		if ((currentel >> 2) != 1)
			panicLogger() << "eir: Unexpected exception level: in EL" << (currentel >> 2)
			              << frg::endlog;
		asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
		eirFlushDisableMmuEl1(flushStart, flushEnd, dcLineSize, sctlr);
	}
}

// CNTHCTL_EL2 and CPTR_EL2 change their layout depending on E2H,
// hence they can only be programmed once we settled on a mode.
// Since the reset value of CNTHCTL_EL2 is UNKNOWN, we never preserve its other bits.

// Must only be called in EL2 with E2H set.
void configureVheTraps() {
	// Do not trap EL0 access to the counters.
	uint64_t cnthctl = 0;
	cnthctl |= UINT64_C(1) << 0; // EL0PCTEN
	cnthctl |= UINT64_C(1) << 1; // EL0VCTEN
	asm volatile("msr cnthctl_el2, %0" : : "r"(cnthctl));

	// Do not trap FP and SIMD. CPTR_EL2 has the CPACR_EL1 layout if E2H is set.
	asm volatile("msr cptr_el2, %0" : : "r"(UINT64_C(0b11) << 20)); // FPEN
}

// Must only be called in EL2 without E2H.
void configureEl2Traps() {
	// Do not trap EL1 access to the counters.
	uint64_t cnthctl = 0;
	cnthctl |= UINT64_C(1) << 0; // EL1PCTEN
	cnthctl |= UINT64_C(1) << 1; // EL1PCEN
	asm volatile("msr cnthctl_el2, %0" : : "r"(cnthctl));

	// Do not trap FP and SIMD to EL2.
	asm volatile("msr cptr_el2, %0" : : "r"(UINT64_C(0x33ff)));
}

// Must only be called in EL2.
// EL1 can only access the GICv3 system registers if EL2 enables the interface for it:
// ICC_SRE_EL1.SRE is RAZ/WI unless ICC_SRE_EL2.SRE is set, and writing ICC_SRE_EL1 traps
// to EL2 unless ICC_SRE_EL2.Enable is set.
void enableGicSysregsForEl1() {
	uint64_t pfr0;
	asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
	if (!((pfr0 >> 24) & 0xF))
		return;

	uint64_t sre;
	asm volatile("mrs %0, icc_sre_el2" : "=r"(sre));
	sre |= UINT64_C(1) << 0; // SRE
	sre |= UINT64_C(1) << 3; // Enable
	asm volatile("msr icc_sre_el2, %0; isb" : : "r"(sre) : "memory");
}

// Must only be called in EL2.
bool inVhe() {
	constexpr uint64_t e2hAndTge = (UINT64_C(1) << 34) | (UINT64_C(1) << 27);
	uint64_t hcr;
	asm volatile("mrs %0, hcr_el2" : "=r"(hcr));
	return (hcr & e2hAndTge) == e2hAndTge;
}

// SCTLR that the kernel runs with.
// Since E2H gives SCTLR_EL2 the SCTLR_EL1 layout, the same value applies to both exception levels.
// The MMU is enabled later on, in enterKernelPaging().
uint64_t kernelSctlr() {
	uint64_t sctlr = 0;
	sctlr |= UINT64_C(1) << 29; // LSMAOE
	sctlr |= UINT64_C(1) << 28; // nTLSMD
	sctlr |= UINT64_C(1) << 23; // SPAN
	sctlr |= UINT64_C(1) << 22; // EIS
	sctlr |= UINT64_C(1) << 20; // TSCXT
	sctlr |= UINT64_C(1) << 12; // I
	sctlr |= UINT64_C(1) << 11; // EOS
	sctlr |= UINT64_C(1) << 2;  // C
	return sctlr;
}

void dropToEl1() {
	asm volatile("msr sctlr_el1, %0" : : "r"(kernelSctlr()));

	uint64_t hcr = 0;
	hcr |= UINT64_C(1) << 1;  // SWIO
	hcr |= UINT64_C(1) << 31; // RW
	asm volatile("msr hcr_el2, %0" : : "r"(hcr));

	// TODO: We keep using the EL2 stack in EL1 here.
	//       We may want to re-evaluate that in the future.
	uint64_t spsr = 0x3c5;
	asm volatile(
	    // clang-format off
	         "adr x0, 1f" "\n"
	    "\t" "msr spsr_el2, %0" "\n"
	    "\t" "msr elr_el2, x0" "\n"
	    "\t" "mov x0, sp" "\n"
	    "\t" "eret" "\n"
	    "1:" "\n"
	    "\t" "mov sp, x0"
	    // clang-format on
	    :
	    : "r"(spsr)
	    : "x0", "memory"
	);
}

// Must only be called in either EL1 or in EL2 with E2H=1.
void enterKernelPaging() {
	uint64_t aa64mmfr0;
	asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(aa64mmfr0));

	// Setup system registers for paging (MAIR and TCR).
	if (((aa64mmfr0 >> 28) & 0xF) == 0xF)
		eir::panicLogger() << "PANIC! This CPU doesn't support 4K memory translation granules"
		                   << frg::endlog;

	auto pa = frg::min(uint64_t(5), aa64mmfr0 & 0xF);

	uint64_t mair = UINT64_C(0b11111111) |         // Normal, Write-back RW-Allocate non-transient
	                (UINT64_C(0b00001100) << 8) |  // Device, GRE
	                (UINT64_C(0b00000000) << 16) | // Device, nGnRnE
	                (UINT64_C(0b00000100) << 24) | // Device, nGnRE
	                (UINT64_C(0b01000100) << 32);  // Normal Non-cacheable

	uint64_t tcr = (16 << 0) |            // T0SZ=16
	               (16 << 16) |           // T1SZ=16
	               (1 << 8) |             // TTBR0 Inner WB RW-Allocate
	               (1 << 10) |            // TTBR0 Outer WB RW-Allocate
	               (1 << 24) |            // TTBR1 Inner WB RW-Allocate
	               (1 << 26) |            // TTBR1 Outer WB RW-Allocate
	               (2 << 12) |            // TTBR0 Inner shareable
	               (2 << 28) |            // TTBR1 Inner shareable
	               (uint64_t(pa) << 32) | // 48-bit intermediate address
	               (uint64_t(2) << 30);   // TTBR1 4K granule

	// TODO: If paging is already enabled, we should not overwrite MAIR and TCR
	//       with potentially conflicting values here.
	//       Instead, ensure that the current values are sane and error out if they are not.
	//       This does not apply if paging is off.
	asm volatile(
	    // clang-format off
		// Reload page table registers.
		     "msr mair_el1, %0" "\n"
		"\t" "msr tcr_el1, %1" "\n"
		"\t" "isb"
	    // clang-format on
	    :
	    : "r"(mair), "r"(tcr)
	    : "memory"
	);

	asm volatile(
	    // clang-format off
		// Reload page table registers.
		     "msr ttbr0_el1, %0" "\n"
		"\t" "msr ttbr1_el1, %1" "\n"
		"\t" "isb" "\n"
		// Invalidate TLB to clear old mappings.
		"\t" "tlbi vmalle1" "\n"
		"\t" "dsb ish" "\n"
		"\t" "isb" "\n"
	    // clang-format on
	    :
	    : "r"(eirTTBR[0] + 1), "r"(eirTTBR[1] + 1)
	    : "memory"
	);

	// Enable the MMU.
	uint64_t sctlr;
	asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr |= UINT64_C(1);

	asm volatile(
	    // clang-format off
		// Reload page table registers.
		     "msr sctlr_el1, %0" "\n"
		"\t" "isb"
	    // clang-format on
	    :
	    : "r"(sctlr)
	    : "memory"
	);
}

} // anonymous namespace

uintptr_t eirTTBR[2];

void setupPaging() {
	eirTTBR[0] = allocPage();
	eirTTBR[1] = allocPage();

	auto ttbr0Ptr = physToVirt<uint64_t>(eirTTBR[0]);
	auto ttbr1Ptr = physToVirt<uint64_t>(eirTTBR[1]);

	for (size_t i = 0; i < 512; i++) {
		ttbr0Ptr[i] = 0;
		ttbr1Ptr[i] = 0;
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
static inline constexpr uint64_t kPageWb = (0 << 2);
static inline constexpr uint64_t kPageGRE = (1 << 2);
static inline constexpr uint64_t kPagenGnRnE = (2 << 2);

void
mapSingle4kPage(address_t address, address_t physical, uint32_t flags, CachingMode cachingMode) {
	auto ttbr = (address >> 63) & 1;
	auto l0 = (address >> 39) & 0x1FF;
	auto l1 = (address >> 30) & 0x1FF;
	auto l2 = (address >> 21) & 0x1FF;
	auto l3 = (address >> 12) & 0x1FF;

	auto l0Tbl = physToVirt<uint64_t>(eirTTBR[ttbr]);
	auto l0Ent = l0Tbl[l0];
	auto l1Ptr = l0Ent & 0xFFFFFFFFF000;
	if (!(l0Ent & kPageValid)) {
		uint64_t addr = allocPage();
		auto ptr = physToVirt<uint64_t>(addr);

		for (int i = 0; i < 512; i++)
			ptr[i] = 0;

		l0Tbl[l0] = addr | kPageValid | kPageTable;
		l1Ptr = addr;
	}

	auto l1Tbl = physToVirt<uint64_t>(l1Ptr);
	auto l1Ent = l1Tbl[l1];
	auto l2Ptr = l1Ent & 0xFFFFFFFFF000;
	if (!(l1Ent & kPageValid)) {
		uint64_t addr = allocPage();
		auto ptr = physToVirt<uint64_t>(addr);

		for (int i = 0; i < 512; i++)
			ptr[i] = 0;

		l1Tbl[l1] = addr | kPageValid | kPageTable;
		l2Ptr = addr;
	}

	auto l2Tbl = physToVirt<uint64_t>(l2Ptr);
	auto l2Ent = l2Tbl[l2];
	auto l3Ptr = l2Ent & 0xFFFFFFFFF000;
	if (!(l2Ent & kPageValid)) {
		uint64_t addr = allocPage();
		auto ptr = physToVirt<uint64_t>(addr);

		for (int i = 0; i < 512; i++)
			ptr[i] = 0;

		l2Tbl[l2] = addr | kPageValid | kPageTable;
		l3Ptr = addr;
	}

	auto l3Tbl = physToVirt<uint64_t>(l3Ptr);
	auto l3Ent = l3Tbl[l3];

	if (l3Ent & kPageValid)
		eir::panicLogger() << "eir: Trying to map 0x" << frg::hex_fmt{address} << " twice!"
		                   << frg::endlog;

	uint64_t newEntry = physical | kPageValid | kPageL3Page | kPageAccess | kPageInnerSh;

	if (!(flags & PageFlags::write))
		newEntry |= kPageRO;
	if (!(flags & PageFlags::execute))
		newEntry |= kPageXN | kPagePXN;
	if (!(flags & PageFlags::global))
		newEntry |= kPageNotGlobal;

	if (cachingMode == CachingMode::writeCombine) {
		newEntry |= kPageGRE;
	} else if (cachingMode == CachingMode::mmio) {
		newEntry |= kPagenGnRnE;
	} else {
		assert(cachingMode == CachingMode::null);
		newEntry |= kPageWb;
	}

	if (newEntry & (0b111ULL << 48)) {
		eir::infoLogger() << "Oops, reserved bits set when mapping 0x" << frg::hex_fmt{physical}
		                  << " to 0x" << frg::hex_fmt{address} << frg::endlog;

		eir::panicLogger() << "New entry value: 0x" << frg::hex_fmt{newEntry} << frg::endlog;
	}

	l3Tbl[l3] = newEntry;
}

address_t getSingle4kPage(address_t address) {
	auto ttbr = (address >> 63) & 1;
	auto l0 = (address >> 39) & 0x1FF;
	auto l1 = (address >> 30) & 0x1FF;
	auto l2 = (address >> 21) & 0x1FF;
	auto l3 = (address >> 12) & 0x1FF;

	auto l0Tbl = physToVirt<uint64_t>(eirTTBR[ttbr]);
	auto l0Ent = l0Tbl[l0];
	auto l1Ptr = l0Ent & 0xFFFFFFFFF000;
	if (!(l0Ent & kPageValid))
		return -1;

	auto l1Tbl = physToVirt<uint64_t>(l1Ptr);
	auto l1Ent = l1Tbl[l1];
	auto l2Ptr = l1Ent & 0xFFFFFFFFF000;
	if (!(l1Ent & kPageValid))
		return -1;

	auto l2Tbl = physToVirt<uint64_t>(l2Ptr);
	auto l2Ent = l2Tbl[l2];
	auto l3Ptr = l2Ent & 0xFFFFFFFFF000;
	if (!(l2Ent & kPageValid))
		return -1;

	auto l3Tbl = physToVirt<uint64_t>(l3Ptr);
	auto l3Ent = l3Tbl[l3];
	auto pagePtr = l3Ent & 0xFFFFFFFFF000;
	if (!(l3Ent & kPageValid))
		return -1;

	return pagePtr;
}

int getKernelVirtualBits() { return 49; }

void initProcessorEarly() {
	uint64_t currentel;
	asm volatile("mrs %0, currentel" : "=r"(currentel));
	eir::infoLogger() << "Starting Eir in EL " << (currentel >> 2) << frg::endlog;

	// Install exception handlers (in case the boot protocol did not do that already).
	auto vbar = reinterpret_cast<void *>(eirExcVectors);
	if ((currentel >> 2) == 1) {
		asm volatile("msr vbar_el1, %0" : : "r"(vbar) : "memory");
	} else {
		assert((currentel >> 2) == 2);
		asm volatile("msr vbar_el2, %0" : : "r"(vbar) : "memory");
	}
}

static initgraph::Task earlyProcessorInit{
    &globalInitEngine,
    "arm.early-processor-init",
    // The firmware handles its own interrupts, hence we must not steal VBAR before it is done.
    initgraph::Requires{getFirmwareDoneStage()},
    initgraph::Entails{getMemoryLayoutReservedStage()},
    [] { initProcessorEarly(); }
};

void initProcessorPaging() {
	setupPaging();
	eir::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10)
	                  << " KiB"
	                     " after setting up paging"
	                  << frg::endlog;
}

bool patchArchSpecificManagarmElfNote(unsigned int, frg::span<char>) { return false; }

[[noreturn]] void enterKernel() {
	uint64_t aa64mmfr1;
	asm volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(aa64mmfr1));

	uint64_t currentel;
	asm volatile("mrs %0, currentel" : "=r"(currentel));
	if ((currentel >> 2) != 1 && (currentel >> 2) != 2)
		panicLogger() << "eir: Unexpected exception level: in EL" << (currentel >> 2)
		              << frg::endlog;

	if ((currentel >> 2) == 2) {
		// Set virtual offset zero.
		asm volatile("msr cntvoff_el2, %0" : : "r"(UINT64_C(0)));

		// TODO: Our previous raspi4 entry code cleared hstr_el2 but it is not clear why.
		asm volatile("msr hstr_el2, %0" : : "r"(UINT64_C(0)));
	}

	if (!physOffset) {
		// Running from identity mapping. Paging may or may not be enabled.
		// Reconfigure paging.
		infoLogger() << "eir: Will reprogram MMU before jumping to kernel" << frg::endlog;

		disableMmu();

		if ((currentel >> 2) == 2) {
			// Enter E2H mode if VHE is supported.
			// Otherwise, drop to EL1.
			if (((aa64mmfr1 >> 8) & 0xF) >= 1) {
				infoLogger() << "eir: Entering VHE mode" << frg::endlog;
				// TGE routes exceptions from EL0 to EL2 and makes EL0 use the EL2&0
				// translation regime. It also redirects the EL1 TLBI instructions to
				// that regime.
				uint64_t hcr = 0;
				hcr |= UINT64_C(1) << 1;  // SWIO
				hcr |= UINT64_C(1) << 27; // TGE
				hcr |= UINT64_C(1) << 31; // RW
				eirEnterE2h(hcr);

				// Only now does SCTLR_EL2 have the SCTLR_EL1 layout.
				asm volatile("msr sctlr_el1, %0; isb" : : "r"(kernelSctlr()) : "memory");

				configureVheTraps();
			} else {
				infoLogger() << "eir: Dropping to EL1 (VHE is unsupported)" << frg::endlog;
				configureEl2Traps();
				enableGicSysregsForEl1();
				dropToEl1();
			}
		}
	} else {
		// We cannot turn off the MMU from a non-identity mapping.
		// If we are in EL2, only continue if VHE is already enabled (as we do not support a drop to
		// EL1 here).
		if ((currentel >> 2) == 2) {
			if (!inVhe())
				panicLogger() << "eir: In EL2 without VHE with non-identity mapping" << frg::endlog;

			// Since the MMU may already be enabled here, we can only set bits and not clear them.
			uint64_t sctlr;
			asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
			asm volatile("msr sctlr_el1, %0; isb" : : "r"(sctlr | kernelSctlr()) : "memory");

			configureVheTraps();
		}

		// Running from non-identity mapping with paging enabled.
		// We cannot reconfigure paging.
		infoLogger()
		    << "eir: Will not reprogram MMU before jumping to kernel (non-identity mapping)"
		    << frg::endlog;
	}

	enterKernelPaging();
	eirEnterKernel(kernelEntry, getKernelStackPtr());
}

} // namespace eir
