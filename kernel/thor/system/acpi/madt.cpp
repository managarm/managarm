#include <algorithm>
#include <frg/optional.hpp>
#include <frg/manual_box.hpp>
#include <frg/vector.hpp>
#include <eir/interface.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/acpi/pm-interface.hpp>
#include <thor-internal/pci/pci.hpp>

#include <lai/core.h>
#include <lai/helpers/pc-bios.h>
#include <lai/helpers/pci.h>
#include <lai/helpers/pm.h>
#include <lai/helpers/sci.h>

namespace thor {
namespace acpi {

// Note: since firmware often provides unaligned MADTs,
//       we just mark all MADT structs as [[gnu::packed]].

struct [[gnu::packed]] MadtHeader {
	uint32_t localApicAddress;
	uint32_t flags;
};

struct [[gnu::packed]] MadtGenericEntry {
	uint8_t type;
	uint8_t length;
};

struct [[gnu::packed]] MadtLocalEntry {
	MadtGenericEntry generic;
	uint8_t processorId;
	uint8_t localApicId;
	uint32_t flags;
};

namespace local_flags {
	static constexpr uint32_t enabled = 1;
};

struct [[gnu::packed]] MadtIoEntry {
	MadtGenericEntry generic;
	uint8_t ioApicId;
	uint8_t reserved;
	uint32_t mmioAddress;
	uint32_t systemIntBase;
};

enum OverrideFlags {
	polarityMask = 0x03,
	polarityDefault = 0x00,
	polarityHigh = 0x01,
	polarityLow = 0x03,

	triggerMask = 0x0C,
	triggerDefault = 0x00,
	triggerEdge = 0x04,
	triggerLevel = 0x0C
};

struct [[gnu::packed]] MadtIntOverrideEntry {
	MadtGenericEntry generic;
	uint8_t bus;
	uint8_t sourceIrq;
	uint32_t systemInt;
	uint16_t flags;
};

struct [[gnu::packed]] MadtLocalNmiEntry {
	MadtGenericEntry generic;
	uint8_t processorId;
	uint16_t flags;
	uint8_t localInt;
};

} } // namespace thor::acpi

// --------------------------------------------------------

namespace thor {

frg::manual_box<frg::optional<GlobalIrqInfo>> isaIrqOverrides[16];

GlobalIrqInfo resolveIsaIrq(unsigned int irq) {
	assert(irq < 16);
	if((*isaIrqOverrides[irq]))
		return *(*isaIrqOverrides[irq]);
	return GlobalIrqInfo{irq, IrqConfiguration{TriggerMode::edge, Polarity::high}};
}

// Same as resolveIsaIrq(irq) but allows to set more specific configuration options.
GlobalIrqInfo resolveIsaIrq(unsigned int irq, IrqConfiguration desired) {
	if(irq < 16 && *isaIrqOverrides[irq]) {
		assert(desired.compatible((*isaIrqOverrides[irq])->configuration));
		return *(*isaIrqOverrides[irq]);
	}
	return GlobalIrqInfo{irq, desired};
}

// --------------------------------------------------------

void configureIrq(GlobalIrqInfo info) {
#ifdef __x86_64__
	auto pin = getGlobalSystemIrq(info.gsi);
	assert(pin);
	pin->configure(info.configuration);
#endif
}

} // namespace thor

// --------------------------------------------------------

namespace thor {
namespace acpi {

struct SciDevice : IrqSink {
	SciDevice()
	: IrqSink{frg::string<KernelAlloc>{*kernelAlloc, "acpi-sci"}} { }

	IrqStatus raise() override {
		auto isr = lai_get_sci_event();
		if(isr & ACPI_POWER_BUTTON)
			lai_enter_sleep(5); // Shut down.

		// Ack the IRQ if any interesting event occurred.
		if(isr & ACPI_POWER_BUTTON)
			return IrqStatus::acked;
		return IrqStatus::nacked;
	}
};

frg::manual_box<SciDevice> sciDevice;

// --------------------------------------------------------

void bootOtherProcessors() {
	void *madtWindow = laihost_scan("APIC", 0);
	assert(madtWindow);
	auto madt = reinterpret_cast<acpi_header_t *>(madtWindow);

	infoLogger() << "thor: Booting APs." << frg::endlog;

	size_t offset = sizeof(acpi_header_t) + sizeof(MadtHeader);
	while(offset < madt->length) {
		auto generic = (MadtGenericEntry *)((uint8_t *)madt + offset);
		if(generic->type == 0) { // local APIC
			auto entry = (MadtLocalEntry *)generic;
			// TODO: Support BSPs with APIC ID != 0.
			if((entry->flags & local_flags::enabled)
					&& entry->localApicId) // We ignore the BSP here.
				bootSecondary(entry->localApicId);
		}
		offset += generic->length;
	}
}

// --------------------------------------------------------

void dumpMadt() {
	void *madtWindow = laihost_scan("APIC", 0);
	assert(madtWindow);
	auto madt = reinterpret_cast<acpi_header_t *>(madtWindow);

	infoLogger() << "thor: Dumping MADT" << frg::endlog;

	size_t offset = sizeof(acpi_header_t) + sizeof(MadtHeader);
	while(offset < madt->length) {
		auto generic = (MadtGenericEntry *)((uintptr_t)madt + offset);
		if(generic->type == 0) { // local APIC
			auto entry = (MadtLocalEntry *)generic;
			infoLogger() << "    Local APIC id: "
					<< (int)entry->localApicId
					<< ((entry->flags & local_flags::enabled) ? "" :" (disabled)")
					<< frg::endlog;
		}else if(generic->type == 1) { // I/O APIC
			auto entry = (MadtIoEntry *)generic;
			infoLogger() << "    I/O APIC id: " << (int)entry->ioApicId
					<< ", sytem interrupt base: " << (int)entry->systemIntBase
					<< frg::endlog;
		}else if(generic->type == 2) { // interrupt source override
			auto entry = (MadtIntOverrideEntry *)generic;

			const char *bus, *polarity, *trigger;
			if(entry->bus == 0) {
				bus = "ISA";
			}else{
				panicLogger() << "Unexpected bus in MADT interrupt override"
						<< frg::endlog;
			}

			if((entry->flags & OverrideFlags::polarityMask) == OverrideFlags::polarityDefault) {
				polarity = "default";
			}else if((entry->flags & OverrideFlags::polarityMask) == OverrideFlags::polarityHigh) {
				polarity = "high";
			}else if((entry->flags & OverrideFlags::polarityMask) == OverrideFlags::polarityLow) {
				polarity = "low";
			}else{
				panicLogger() << "Unexpected polarity in MADT interrupt override"
						<< frg::endlog;
			}

			if((entry->flags & OverrideFlags::triggerMask) == OverrideFlags::triggerDefault) {
				trigger = "default";
			}else if((entry->flags & OverrideFlags::triggerMask) == OverrideFlags::triggerEdge) {
				trigger = "edge";
			}else if((entry->flags & OverrideFlags::triggerMask) == OverrideFlags::triggerLevel) {
				trigger = "level";
			}else{
				panicLogger() << "Unexpected trigger mode in MADT interrupt override"
						<< frg::endlog;
			}

			infoLogger() << "    Int override: " << bus << " IRQ " << (int)entry->sourceIrq
					<< " is mapped to GSI " << entry->systemInt
					<< " (Polarity: " << polarity << ", trigger mode: " << trigger
					<< ")" << frg::endlog;
		}else if(generic->type == 4) { // local APIC NMI source
			auto entry = (MadtLocalNmiEntry *)generic;
			infoLogger() << "    Local APIC NMI: processor " << (int)entry->processorId
					<< ", lint: " << (int)entry->localInt << frg::endlog;
		}else{
			infoLogger() << "    Unexpected MADT entry of type "
					<< generic->type << frg::endlog;
		}
		offset += generic->length;
	}
}

void *globalRsdtWindow;
int globalRsdtVersion;

extern "C" EirInfo *thorBootInfoPtr;

initgraph::Stage *getTablesDiscoveredStage() {
	static initgraph::Stage s{&globalInitEngine, "acpi.tables-discovered"};
	return &s;
}

initgraph::Stage *getNsAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "acpi.ns-available"};
	return &s;
}

static initgraph::Task initTablesTask{&globalInitEngine, "acpi.init-tables",
	initgraph::Entails{getTablesDiscoveredStage()},
	[] {
		lai_rsdp_info rsdp_info;
		if(thorBootInfoPtr->acpiRsdt) {
			if(thorBootInfoPtr->acpiRevision == 1) {
				rsdp_info.acpi_version = 1;
				rsdp_info.rsdt_address = thorBootInfoPtr->acpiRsdt;
				rsdp_info.xsdt_address = 0;
			} else if(thorBootInfoPtr->acpiRevision == 2) {
				rsdp_info.acpi_version = 2;
				rsdp_info.rsdt_address = 0;
				rsdp_info.xsdt_address = thorBootInfoPtr->acpiRsdt;
			} else {
				panicLogger() << "thor: Got unknown acpi version from multiboot2: " << thorBootInfoPtr->acpiRevision << frg::endlog;
			}
		} else {
			if(lai_bios_detect_rsdp(&rsdp_info))
				panicLogger() << "thor: Could not detect ACPI" << frg::endlog;
		}

		assert((rsdp_info.acpi_version == 1 || rsdp_info.acpi_version == 2) && "Got unknown acpi version from lai");
		globalRsdtVersion = rsdp_info.acpi_version;
		if(rsdp_info.acpi_version == 2){
			globalRsdtWindow = laihost_map(rsdp_info.xsdt_address, 0x1000);
			auto xsdt = reinterpret_cast<acpi_rsdt_t *>(globalRsdtWindow);
			globalRsdtWindow = laihost_map(rsdp_info.xsdt_address, xsdt->header.length);
		} else if(rsdp_info.acpi_version == 1) {
			globalRsdtWindow = laihost_map(rsdp_info.rsdt_address, 0x1000);
			auto rsdt = reinterpret_cast<acpi_rsdt_t *>(globalRsdtWindow);
			globalRsdtWindow = laihost_map(rsdp_info.rsdt_address, rsdt->header.length);
		}

		lai_create_namespace();
	}
};

static initgraph::Task discoverIoApicsTask{&globalInitEngine, "acpi.discover-ioapics",
	initgraph::Requires{getTablesDiscoveredStage(),
		getFibersAvailableStage()},
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		dumpMadt();

		void *madtWindow = laihost_scan("APIC", 0);
		assert(madtWindow);
		auto madt = reinterpret_cast<acpi_header_t *>(madtWindow);

		// Configure all interrupt controllers.
		// TODO: This should be done during thor's initialization in order to avoid races.
		infoLogger() << "thor: Configuring I/O APICs." << frg::endlog;

		size_t offset = sizeof(acpi_header_t) + sizeof(MadtHeader);
		while(offset < madt->length) {
			auto generic = (MadtGenericEntry *)((uint8_t *)madtWindow + offset);
			if(generic->type == 1) { // I/O APIC
				auto entry = (MadtIoEntry *)generic;
#ifdef __x86_64__
				setupIoApic(entry->ioApicId, entry->systemIntBase, entry->mmioAddress);
#endif
			}
			offset += generic->length;
		}

		// Determine IRQ override configuration.
		for(int i = 0; i < 16; i++)
			isaIrqOverrides[i].initialize();

		offset = sizeof(acpi_header_t) + sizeof(MadtHeader);
		while(offset < madt->length) {
			auto generic = (MadtGenericEntry *)((uint8_t *)madtWindow + offset);
			if(generic->type == 2) { // interrupt source override
				auto entry = (MadtIntOverrideEntry *)generic;

				// ACPI defines only ISA IRQ overrides.
				assert(entry->bus == 0);
				assert(entry->sourceIrq < 16);

				GlobalIrqInfo line;
				line.gsi = entry->systemInt;

				auto trigger = entry->flags & OverrideFlags::triggerMask;
				auto polarity = entry->flags & OverrideFlags::polarityMask;
				if(trigger == OverrideFlags::triggerDefault
						&& polarity == OverrideFlags::polarityDefault) {
					line.configuration.trigger = TriggerMode::edge;
					line.configuration.polarity = Polarity::high;
				}else{
					assert(trigger != OverrideFlags::triggerDefault);
					assert(polarity != OverrideFlags::polarityDefault);

					switch(trigger) {
					case OverrideFlags::triggerEdge:
						line.configuration.trigger = TriggerMode::edge; break;
					case OverrideFlags::triggerLevel:
						line.configuration.trigger = TriggerMode::level; break;
					default:
						panicLogger() << "Illegal IRQ trigger mode in MADT" << frg::endlog;
					}

					switch(polarity) {
					case OverrideFlags::polarityHigh:
						line.configuration.polarity = Polarity::high; break;
					case OverrideFlags::polarityLow:
						line.configuration.polarity = Polarity::low; break;
					default:
						panicLogger() << "Illegal IRQ polarity in MADT" << frg::endlog;
					}
				}

				assert(!(*isaIrqOverrides[entry->sourceIrq]));
				*isaIrqOverrides[entry->sourceIrq] = line;
			}
			offset += generic->length;
		}
	}
};

static initgraph::Task enterAcpiModeTask{&globalInitEngine, "acpi.enter-acpi-mode",
	initgraph::Requires{getTaskingAvailableStage(),
		pci::getBus0AvailableStage()},
	initgraph::Entails{getNsAvailableStage()},
	[] {
		// Configure the ISA IRQs.
		// TODO: This is a hack. We assume that HPET will use legacy replacement.
		infoLogger() << "thor: Configuring ISA IRQs." << frg::endlog;
		configureIrq(resolveIsaIrq(0));
		configureIrq(resolveIsaIrq(1));
		configureIrq(resolveIsaIrq(4));
		configureIrq(resolveIsaIrq(12));
		configureIrq(resolveIsaIrq(14));

		// Install the SCI before enabling ACPI.
		void *fadtWindow = laihost_scan("FACP", 0);
		assert(fadtWindow);
		auto fadt = reinterpret_cast<acpi_fadt_t *>(fadtWindow);

		auto sciOverride = resolveIsaIrq(fadt->sci_irq);
		configureIrq(sciOverride);
		sciDevice.initialize();
		lai_set_sci_event(ACPI_POWER_BUTTON);
#ifdef __x86_64__
		IrqPin::attachSink(getGlobalSystemIrq(sciOverride.gsi), sciDevice.get());
#endif

		// Enable ACPI.
		infoLogger() << "thor: Entering ACPI mode." << frg::endlog;
		lai_enable_acpi(1);
		infoLogger() << "thor: ACPI configuration complete." << frg::endlog;
	}
};

static initgraph::Task bootApsTask{&globalInitEngine, "acpi.boot-aps",
	initgraph::Requires{&enterAcpiModeTask},
	[] {
		bootOtherProcessors();
	}
};

static initgraph::Task initPmInterfaceTask{&globalInitEngine, "acpi.init-pm-interface",
	initgraph::Requires{&enterAcpiModeTask},
	[] {
		initializePmInterface();
	}
};

} } // namespace thor::acpi
