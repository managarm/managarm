#include <frg/optional.hpp>
#include <frg/scope_exit.hpp>
#include <frg/vector.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch/gic.hpp>
#include <thor-internal/arch/gic_v2.hpp>
#include <thor-internal/arch/gic_v3.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/int-call.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/schedule.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/traps.hpp>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

namespace thor {

uint32_t affinityFromMpidr(uint64_t mpidr) {
	return (mpidr & 0xFFFFFF) | ((mpidr >> 32) & 0xFF) << 24;
}

namespace {

void installAcpiGsiPins() {
	std::visit(
	    frg::overloaded{
	        [](GicV2 *gic) {
		        for (uint32_t irq = 16; irq < gic->irqCount(); ++irq) {
			        auto pin = gic->getPin(irq);
			        if (pin)
				        acpi::setGlobalSystemIrq(irq, pin);
		        }
	        },
	        [](GicV3 *gic) {
		        for (uint32_t irq = 16; irq < gic->irqCount(); ++irq) {
			        auto pin = gic->getPin(irq);
			        if (pin)
				        acpi::setGlobalSystemIrq(irq, pin);
		        }
	        },
	        [](auto &&) {}
	    },
	    externalIrq
	);
}

bool initGicFromAcpi() {
	if (!acpiRsdpNote->rsdp)
		return false;

	uacpi_table madtTbl;
	if (uacpi_table_find_by_signature("APIC", &madtTbl) != UACPI_STATUS_OK)
		return false;
	frg::scope_exit finish{[&] { uacpi_table_unref(&madtTbl); }};

	auto *madt = reinterpret_cast<acpi_madt *>(madtTbl.ptr);
	auto bspAffinity = getCpuData()->affinity;

	frg::optional<acpi_madt_gicd> gicd;
	uintptr_t cpuInterface = 0;
	frg::vector<GicRedistributorRange, KernelAlloc> gicrRanges{*kernelAlloc};
	frg::vector<GicRedistributorRange, KernelAlloc> giccRedistRanges{*kernelAlloc};

	size_t offset = sizeof(acpi_madt);
	while (offset < madt->hdr.length) {
		acpi_entry_hdr generic;
		auto genericPtr = reinterpret_cast<void *>(madtTbl.virt_addr + offset);
		memcpy(&generic, genericPtr, sizeof(generic));

		switch (generic.type) {
			case ACPI_MADT_ENTRY_TYPE_GICD: {
				acpi_madt_gicd entry;
				memcpy(&entry, genericPtr, sizeof(entry));
				gicd = entry;
			} break;
			case ACPI_MADT_ENTRY_TYPE_GICC: {
				acpi_madt_gicc entry;
				memcpy(&entry, genericPtr, sizeof(entry));
				if (!(entry.flags & ACPI_GICC_ENABLED))
					break;

				if (affinityFromMpidr(entry.mpidr) == bspAffinity)
					cpuInterface = entry.address;
				if (entry.gicr_base_address)
					giccRedistRanges.push({entry.gicr_base_address, 0x20000});
			} break;
			case ACPI_MADT_ENTRY_TYPE_GICR: {
				acpi_madt_gicr entry;
				memcpy(&entry, genericPtr, sizeof(entry));
				gicrRanges.push({entry.address, entry.length});
			} break;
			default:
				break;
		}

		offset += generic.length;
	}

	if (!gicd) {
		warningLogger() << "thor: ACPI MADT has no GIC distributor" << frg::endlog;
		return false;
	}

	auto version = gicd->gic_version;
	if (!version)
		version = (gicrRanges.size() || giccRedistRanges.size()) ? 3 : 2;

	if (version >= 3) {
		if (gicrRanges.size())
			return initGicV3FromAcpi(
			    gicd->address,
			    0x10000,
			    frg::span<const GicRedistributorRange>{gicrRanges.data(), gicrRanges.size()}
			);
		return initGicV3FromAcpi(
		    gicd->address,
		    0x10000,
		    frg::span<const GicRedistributorRange>{giccRedistRanges.data(), giccRedistRanges.size()}
		);
	}

	if (!cpuInterface) {
		warningLogger() << "thor: ACPI MADT has no BSP GICC CPU interface" << frg::endlog;
		return false;
	}

	return initGicV2FromAcpi(gicd->address, cpuInterface, 0x2000);
}

} // namespace

static initgraph::Task initGic{
    &globalInitEngine,
    "arm.init-gic",
    initgraph::Requires{
        acpi::getTablesDiscoveredStage(), getDeviceTreeParsedStage(), getBootProcessorReadyStage()
    },
    initgraph::Entails{getIrqControllerReadyStage()},
    // Initialize the GIC.
    [] {
	    bool acpiGic = initGicFromAcpi();
	    if (acpiGic || initGicV2() || initGicV3()) {
		    initGicOnThisCpu();
		    if (acpiGic)
			    installAcpiGsiPins();
	    }
    }
};

void initGicOnThisCpu() {
	if (std::holds_alternative<GicV2 *>(externalIrq)) {
		initGicOnThisCpuV2();
	} else if (std::holds_alternative<GicV3 *>(externalIrq)) {
		initGicOnThisCpuV3();
	}
}

constexpr bool logSGIs = false;
constexpr bool logSpurious = false;

void handleIrq(IrqImageAccessor image, IrqPin *irq);

void handleGicIrq(IrqImageAccessor image, ClaimedExternalIrq irq) {
	auto *cpuData = getCpuData();

	if (irq.irq < 16) {
		if constexpr (logSGIs) {
			infoLogger() << "thor: handleGicIrq: on CPU " << cpuData->cpuIndex << ", got an SGI "
			             << irq.irq << frg::endlog;
		}

		std::visit(
		    frg::overloaded{
		        [&](GicV2 *gic) { gic->eoi(irq.cpu, irq.irq); },
		        [&](GicV3 *gic) { gic->eoi(irq.cpu, irq.irq); },
		        [](auto &&) {
			        // How did we even get here..?
			        __builtin_trap();
		        }
		    },
		    externalIrq
		);

		if (irq.irq == 0) {
			localScheduler.get(cpuData).forcePreemptionCall();
		} else if (irq.irq == 1) {
			assert(!irqMutex().nesting());
			disableUserAccess();

			for (auto &binding : asidData.get()->bindings) {
				binding.shootdown();
			}

			asidData.get()->globalBinding.shootdown();
		} else if (irq.irq == 2) {
			assert(!irqMutex().nesting());
			disableUserAccess();

			SelfIntCallBase::runScheduledCalls();
		} else {
			panicLogger() << "thor: handleGicIrq: Received unexpected SGI " << irq.irq
			              << frg::endlog;
		}

		if (image.inUserMode()) {
			auto thisThread = getCurrentThread();
			assert(thisThread);

			localScheduler.get(cpuData).checkPreemption();

			if (thisThread->checkConditions()) {
				iplDemoteContext(ipl::passive);
				enableInts();

				StatelessIrqLock irqLock(frg::dont_lock);
				handleThreadReturnToUserMode(image, irqLock);
				irqLock.release();
			}
		} else {
			localScheduler.get(cpuData).checkPreemption(image);
		}
	} else if (irq.irq >= 1020) {
		if constexpr (logSpurious) {
			infoLogger() << "thor: handleGicIrq: on CPU " << cpuData->cpuIndex
			             << ", got a spurious IRQ " << irq.irq << frg::endlog;
		}
	} else {
		handleIrq(image, irq.pin);

		if (image.inUserMode()) {
			auto thisThread = getCurrentThread();
			assert(thisThread);

			localScheduler.get(cpuData).checkPreemption();

			if (thisThread->checkConditions()) {
				iplDemoteContext(ipl::passive);
				enableInts();

				StatelessIrqLock irqLock(frg::dont_lock);
				handleThreadReturnToUserMode(image, irqLock);
				irqLock.release();
			}
		} else {
			localScheduler.get(cpuData).checkPreemption(image);
		}
	}
}

} // namespace thor
