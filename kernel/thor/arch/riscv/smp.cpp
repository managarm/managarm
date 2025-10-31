#include <riscv/sbi.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/ring-buffer.hpp>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

namespace thor {

extern "C" uint8_t thorSmpTrampolineStart[];
extern "C" uint8_t thorSmpTrampolineEnd[];

namespace {

const frg::array<frg::string_view, 1> cpuCompatible = {"riscv"};

// RISC-V AP initialization is quite pleasant to work with.
// In particular, SBI allows us to pass an opaque pointer to the SMP entry point.
// Hence, we do not need to embed the StatusBlock pointer into the entry stub
// and we can start all APs using the same code path (even in parallel).

// Global helper struct that we embed into the entry stub.
// Note: this struct needs to be synchronized with assembly.
struct TrampolineHeader {
	uint64_t satp;
};

// Per-AP helper struct that we pass as an opaque pointer to SBI.
// Note: this struct needs to be synchronized with assembly.
struct StatusBlock {
	void *sp{nullptr};
	void (*entry)(StatusBlock *){nullptr};
	CpuData *smpCpu{nullptr};
	UniqueKernelStack stack;
};

constinit PhysicalAddr smpTrampolinePage{};
constinit frg::manual_box<ClientPageSpace> smpPageSpace;

void setUpTrampoline() {
	// Allocate a page and create a lower half mapping to identity map it.
	smpTrampolinePage = physicalAllocator->allocate(kPageSize);

	smpPageSpace.initialize();
	ClientPageSpace::Cursor cursor{smpPageSpace.get(), smpTrampolinePage};
	cursor.map4k(smpTrampolinePage, page_access::execute, CachingMode::null);
	*cursor.getPtePtr() &= ~pteUser;  // Workaround: unset the U bit such that S-mode can execute.
	*cursor.getPtePtr() |= pteAccess; // Workaround: set the A bit to avoid a page fault.

	// Copy the trampoline and setup satp.
	// Trampoline page starts with a uint64_t that contains the satp value.
	auto trampolinePtr = reinterpret_cast<char *>(mapDirectPhysical(smpTrampolinePage));
	memcpy(
	    trampolinePtr + sizeof(TrampolineHeader),
	    thorSmpTrampolineStart,
	    reinterpret_cast<uintptr_t>(thorSmpTrampolineEnd)
	        - reinterpret_cast<uintptr_t>(thorSmpTrampolineStart)
	);
	uint64_t mode = 8 + (ClientCursorPolicy::numLevels() - 3);
	new (trampolinePtr) TrampolineHeader{.satp = (smpPageSpace->rootTable() >> 12) | (mode << 60)};
}

void smpMain(StatusBlock *statusBlock) {
	// Synchronize with the other HART.
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	writeToTp(statusBlock->smpCpu);

	initializeThisProcessor();

	runOnStack(
	    [](Continuation, StatusBlock *statusBlock) {
		    infoLogger() << "Hello world on CPU #" << getCpuData()->cpuIndex << frg::endlog;

		    // Note: this will destroy the stack that smpMain ran on.
		    //       It needs to happen on the detached stack!
		    frg::destruct(*kernelAlloc, statusBlock);

		    Scheduler::resume(getCpuData()->wqFiber);

		    LoadBalancer::singleton().setOnline(getCpuData());
		    auto *scheduler = &localScheduler.get();
		    scheduler->update();
		    scheduler->forceReschedule();
		    scheduler->commitReschedule();
	    },
	    getCpuData()->detachedStack.base(),
	    statusBlock
	);
}

void bootAp(uint64_t hartId) {
	// Skip the BSP.
	if (hartId == getCpuData()->hartId)
		return;

	// Setup the CpuData.
	auto [smpCpu, cpuNr] = extendPerCpuData();
	prepareCpuDataFor(smpCpu, cpuNr);
	smpCpu->hartId = hartId;
	// Ensure that the CPU data is visible to the HART.
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	smpCpu->localLogRing = frg::construct<ReentrantRecordRing>(*kernelAlloc);

	// Participate in global TLB invalidation *before* paging is used by the target CPU.
	initializeAsidContext(smpCpu);

	// Setup the stack and related data.
	auto statusBlock = frg::construct<StatusBlock>(*kernelAlloc);
	auto stack = UniqueKernelStack::make();
	auto sp = stack.basePtr();

	statusBlock->sp = sp;
	statusBlock->entry = smpMain;
	statusBlock->smpCpu = smpCpu;
	statusBlock->stack = std::move(stack);

	// Finally call into SBI to boot the hart.
	// Since SBI guarantees on success that the CPU boots, we do not need to wait for smpMain.
	infoLogger() << "Booting hart with hart ID " << hartId << frg::endlog;
	auto sbiError = sbi::hsm::hartStart(
	    hartId,
	    smpTrampolinePage + sizeof(TrampolineHeader),
	    reinterpret_cast<uintptr_t>(statusBlock)
	);
	if (sbiError)
		panicLogger() << "SBI HSM hart start failed with error " << sbiError << frg::endlog;
}

void bootApFromDt(DeviceTreeNode *node) {
	const auto &reg = node->reg();
	if (reg.size() != 1)
		panicLogger() << "thor: Expect exactly one 'reg' entry for RISC-V CPUs" << frg::endlog;
	bootAp(reg.front().addr);
}

initgraph::Task initAPsAcpi{
    &globalInitEngine,
    "riscv.init-aps-acpi",
    initgraph::Requires{acpi::getTablesDiscoveredStage(), getTaskingAvailableStage()},
    [] {
	    if (!getEirInfo()->acpiRsdp)
		    return;

	    setUpTrampoline();

	    uacpi_table madtTbl;
	    if (uacpi_table_find_by_signature("APIC", &madtTbl) != UACPI_STATUS_OK)
		    panicLogger() << "thor: Unable to initalize APs, no MADT found" << frg::endlog;
	    auto *madt = madtTbl.hdr;

	    infoLogger() << "thor: Booting APs." << frg::endlog;

	    size_t offset = sizeof(acpi_madt);
	    while (offset < madt->length) {
		    acpi_entry_hdr generic;
		    auto genericPtr = (void *)(madtTbl.virt_addr + offset);
		    memcpy(&generic, genericPtr, sizeof(generic));
		    switch (generic.type) {
			    case 0x18: {
				    acpi_madt_rintc entry;
				    memcpy(&entry, genericPtr, sizeof(acpi_madt_rintc));
				    infoLogger() << "Booting " << entry.hart_id << frg::endlog;
				    bootAp(entry.hart_id);
			    } break;
		    }
		    offset += generic.length;
	    }
    }
};

initgraph::Task initAPs{
    &globalInitEngine,
    "riscv.init-aps",
    initgraph::Requires{getDeviceTreeParsedStage(), getTaskingAvailableStage()},
    [] {
	    auto root = getDeviceTreeRoot();
	    if (!root)
		    return;

	    setUpTrampoline();

	    if (root) {
		    root->forEach([&](DeviceTreeNode *node) -> bool {
			    if (node->isCompatible(cpuCompatible))
				    bootApFromDt(node);
			    return false;
		    });
	    }
    }
};

} // namespace

} // namespace thor
