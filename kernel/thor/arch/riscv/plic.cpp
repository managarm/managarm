#include <frg/dyn_array.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/dtb/irq.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/physical.hpp>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

namespace thor {

namespace {

const frg::array<frg::string_view, 1> plicCompatible = {"riscv,plic0"};
const frg::array<frg::string_view, 1> intcCompatible = {"riscv,cpu-intc"};
const frg::array<frg::string_view, 1> cpuCompatible = {"riscv"};

// Per interrupt-source registers.
constexpr auto plicPriorityRegister(size_t n) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(4 * n)};
}

// Per (context, interrupt-source) registers.
constexpr auto plicEnableRegister(size_t ctx, size_t n) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x2000 + 0x80 * ctx + 4 * n)};
}

// Per context registers.
constexpr auto plicThresholdRegister(size_t ctx) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x200000 + 0x1000 * ctx)};
}
constexpr auto plicClaimCompleteRegister(size_t ctx) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x200000 + 0x1000 * ctx + 4)};
}

struct Plic : dt::IrqController {
	static frg::string<KernelAlloc> buildName(Plic *plic, size_t idx) {
		return frg::string<KernelAlloc>{*kernelAlloc, "plic@"}
		       + frg::to_allocated_string(*kernelAlloc, plic->base_, 16)
		       + frg::string<KernelAlloc>{*kernelAlloc, ":"}
		       + frg::to_allocated_string(*kernelAlloc, idx);
	}

	struct Irq final : IrqPin {
		Irq(Plic *plic, size_t idx) : IrqPin{buildName(plic, idx)}, plic_{plic}, idx_{idx} {
			assert(idx_);
		}

		Irq(const Irq &) = delete;
		Irq &operator=(const Irq &) = delete;

		IrqStrategy program(TriggerMode mode, Polarity polarity) override {
			unmask();
			return irq_strategy::maskable | irq_strategy::endOfService;
		}

		void mask() override { plic_->mask(plic_->bspCtx_, idx_); }
		void unmask() override { plic_->unmask(plic_->bspCtx_, idx_); }

		// The PLIC does not know whether interrupts are edge- or level-triggered.
		// We can handle both cases transparently by sending completion only when
		// an interrupt is serviced successfully.
		void endOfService() override { plic_->complete(plic_->bspCtx_, idx_); }

	private:
		Plic *plic_;
		size_t idx_;
	};

	Plic(PhysicalAddr base, size_t size, size_t numIrqs, size_t bspCtx)
	: base_{base},
	  size_{size},
	  bspCtx_{bspCtx} {
		auto ptr = KernelVirtualMemory::global().allocate(size);
		for (size_t i = 0; i < size; i += kPageSize) {
			KernelPageSpace::global().mapSingle4k(
			    reinterpret_cast<uintptr_t>(ptr) + i,
			    base + i,
			    page_access::write,
			    CachingMode::mmio
			);
		}
		space_ = arch::mem_space{ptr};

		irqs_ = {numIrqs, *kernelAlloc};
		irqs_[0] = nullptr;
		for (size_t i = 1; i < numIrqs; ++i)
			irqs_[i] = frg::construct<Irq>(*kernelAlloc, this, i);

		// Set all IRQs to the highest priority.
		for (size_t i = 1; i < numIrqs; ++i)
			space_.store(plicPriorityRegister(i), 0xFFFF'FFFF);

		// Accept IRQs of any priority.
		space_.store(plicThresholdRegister(bspCtx_), 0);
	}

	Plic(const Plic &) = delete;
	Plic &operator=(const Plic &) = delete;

	IrqPin *getIrq(size_t idx) {
		assert(idx < irqs_.size());
		return irqs_[idx];
	}

	IrqPin *resolveDtIrq(dtb::Cells irqSpecifier) override {
		if (irqSpecifier.numCells() != 1)
			panicLogger() << "PLIC #interrupt-cells should be 1" << frg::endlog;
		uint32_t idx;
		if (!irqSpecifier.read(idx))
			panicLogger() << "Failed to read PLIC interrupt specifier" << frg::endlog;

		auto pin = getIrq(idx);
		// PLIC does not care about trigger mode / polarity.
		pin->configure({TriggerMode::edge, Polarity::high});
		return pin;
	}

	// Claim the highest priority interrupt.
	uint32_t claim(size_t ctx) { return space_.load(plicClaimCompleteRegister(ctx)); }
	// Complete IRQ handling. Called at EOI.
	void complete(size_t ctx, size_t idx) { space_.store(plicClaimCompleteRegister(ctx), idx); }

	void mask(size_t ctx, size_t idx) {
		auto reg = plicEnableRegister(ctx, idx >> 5);
		auto bits = space_.load(reg);
		bits &= ~(UINT32_C(1) << (idx & 0x1F));
		space_.store(reg, bits);
	}

	void unmask(size_t ctx, size_t idx) {
		auto reg = plicEnableRegister(ctx, idx >> 5);
		auto bits = space_.load(reg);
		bits |= UINT32_C(1) << (idx & 0x1F);
		space_.store(reg, bits);
	}

private:
	PhysicalAddr base_;
	size_t size_;
	arch::mem_space space_;
	frg::dyn_array<Irq *, KernelAlloc> irqs_{*kernelAlloc};
	// TODO: The current implementation routes all IRQs to the BSP.
	//       We should allow routing of IRQs to other harts as well.
	size_t bspCtx_;
};

void enumeratePlicFromDt(DeviceTreeNode *plicNode) {
	const auto &reg = plicNode->reg();
	if (reg.size() != 1)
		panicLogger() << "thor: Expect exactly one 'reg' entry for PLICs" << frg::endlog;

	size_t bspCtx{~size_t{0}};
	size_t i = 0;
	bool success = dt::walkInterruptsExtended(
	    [&](DeviceTreeNode *intcNode, dtb::Cells intcIrq) {
		    if (!intcNode->isCompatible(intcCompatible))
			    panicLogger() << "Expected interrupt parent of PLIC to be cpu-intc device"
			                  << frg::endlog;

		    // Find the CPU of the PLIC context based on the cpu-intc node.
		    auto cpuNode = intcNode->parent();
		    if (!cpuNode || !cpuNode->isCompatible(cpuCompatible))
			    panicLogger() << "Expected parent of cpu-intc device to be CPU" << frg::endlog;
		    auto hartId = cpuNode->reg().front().addr;

		    // Get the IRQ index at the parent cpu-intc.
		    uint32_t intcIdx;
		    if (!intcIrq.read(intcIdx))
			    panicLogger() << "Failed to read cpu-intc interrupt index" << frg::endlog;
		    // -1 means that the PLIC context is not present (see PLIC DT bindings).
		    if (intcIdx != ~UINT32_C(0))
			    infoLogger() << "thor: Context " << i << " connected to hart ID " << hartId
			                 << ", interrupt " << intcIdx << frg::endlog;

		    if (hartId == getCpuData()->hartId && intcIdx == riscv::interrupts::sei)
			    bspCtx = i;
		    ++i;
	    },
	    plicNode
	);
	if (!success)
		panicLogger() << "Failed to walk interrupts of " << plicNode->path() << frg::endlog;

	if (bspCtx == ~size_t{0})
		panicLogger() << "Failed to determine PLIC context of BSP" << frg::endlog;
	infoLogger() << "thor: Context " << bspCtx << " connected to BSP S-mode external interrupt"
	             << frg::endlog;

	auto ndevProp = plicNode->dtNode().findProperty("riscv,ndev");
	if (!ndevProp)
		panicLogger() << "thor: PLIC has no riscv,ndev property" << frg::endlog;
	uint32_t ndev;
	if (!ndevProp->access().readCells(ndev, 1))
		panicLogger() << "thor: Failed to read riscv,ndev from PLIC" << frg::endlog;

	auto plic =
	    frg::construct<Plic>(*kernelAlloc, reg.front().addr, reg.front().size, ndev, bspCtx);
	plicNode->associateIrqController(plic);

	auto *ourExternalIrq = &riscvExternalIrq.get();
	ourExternalIrq->type = ExternalIrqType::plic;
	ourExternalIrq->controller = plic;
	ourExternalIrq->context = bspCtx;
}

void enumeratePlicFromAcpi() {
	uacpi_table madtTbl;
	if (uacpi_table_find_by_signature("APIC", &madtTbl) != UACPI_STATUS_OK)
		return;
	auto *madt = madtTbl.hdr;

	// First, iterate the MADT and find the PLIC context for this HART.
	size_t offset = sizeof(acpi_madt);
	size_t ctx = ~size_t(0);
	size_t plicId = ~size_t(0);

	while (offset < madt->length) {
		acpi_entry_hdr generic;
		auto genericPtr = (void *)(madtTbl.virt_addr + offset);
		memcpy(&generic, genericPtr, sizeof(generic));
		if (generic.type == ACPI_MADT_ENTRY_TYPE_RINTC) {
			acpi_madt_rintc entry;
			memcpy(&entry, genericPtr, sizeof(acpi_madt_rintc));

			if (entry.hart_id == getCpuData()->hartId) {
				ctx = entry.ext_intc_id & 0xFFFF;
				plicId = entry.ext_intc_id >> 24;
			}
		}
		offset += generic.length;
	}

	if (ctx == ~size_t(0)) {
		warningLogger() << "thor: Could not get the PLIC context from the MADT" << frg::endlog;
		return;
	}
	if (plicId == ~size_t(0)) {
		warningLogger() << "thor: Could not get the PLIC ID from the MADT" << frg::endlog;
		return;
	}

	// Then do it again for the actual PLIC.
	offset = sizeof(acpi_madt);
	while (offset < madt->length) {
		acpi_entry_hdr generic;
		auto genericPtr = (void *)(madtTbl.virt_addr + offset);
		memcpy(&generic, genericPtr, sizeof(generic));
		if (generic.type == ACPI_MADT_ENTRY_TYPE_PLIC) {
			acpi_madt_plic entry;
			memcpy(&entry, genericPtr, sizeof(acpi_madt_plic));

			if (entry.id != plicId) {
				warningLogger() << "thor: Got PLIC " << entry.id
				                << " that's not covered by the BSP!" << frg::endlog;
				continue;
			}

			auto plic = frg::construct<Plic>(
			    *kernelAlloc, entry.address, entry.size, entry.sources_count, ctx
			);

			auto *ourExternalIrq = &riscvExternalIrq.get();
			ourExternalIrq->type = ExternalIrqType::plic;
			ourExternalIrq->controller = plic;
			ourExternalIrq->context = ctx;

			infoLogger() << "thor: Installing " << entry.sources_count
			             << " system IRQs for PLIC at " << frg::hex_fmt{entry.address}
			             << frg::endlog;

			for (size_t i = 0; i < entry.sources_count; i++) {
				acpi::setGlobalSystemIrq(entry.gsi_base + i, plic->getIrq(i));
			}
		}
		offset += generic.length;
	}
}

initgraph::Task initPlicAcpi{
    &globalInitEngine,
    "riscv.init-plic-acpi",
    initgraph::Requires{acpi::getTablesDiscoveredStage()},
    initgraph::Entails{getTaskingAvailableStage()},
    [] {
	    auto rsdp = getEirInfo()->acpiRsdp;
	    if (!rsdp)
		    return;

	    enumeratePlicFromAcpi();
    }
};

initgraph::Task initPlic{
    &globalInitEngine,
    "riscv.init-plic",
    initgraph::Requires{getDeviceTreeParsedStage()},
    initgraph::Entails{getTaskingAvailableStage()},
    [] {
	    auto root = getDeviceTreeRoot();
	    if (!root)
		    return;

	    root->forEach([&](DeviceTreeNode *node) -> bool {
		    if (node->isCompatible(plicCompatible))
			    enumeratePlicFromDt(node);
		    return false;
	    });
    }
};

} // namespace

IrqPin *claimPlicIrq() {
	auto *ourExternalIrq = &riscvExternalIrq.get();
	assert(ourExternalIrq->type == ExternalIrqType::plic);
	assert(ourExternalIrq->controller);
	auto *plic = static_cast<Plic *>(ourExternalIrq->controller);
	auto idx = plic->claim(ourExternalIrq->context);
	if (!idx)
		return nullptr;
	return plic->getIrq(idx);
}

} // namespace thor
