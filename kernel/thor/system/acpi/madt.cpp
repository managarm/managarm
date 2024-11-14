#include <algorithm>
#include <frg/optional.hpp>
#include <frg/manual_box.hpp>
#include <frg/vector.hpp>
#include <eir/interface.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/acpi/pm-interface.hpp>
#include <thor-internal/pci/pci.hpp>

#include <uacpi/acpi.h>
#include <uacpi/utilities.h>
#include <uacpi/tables.h>
#include <uacpi/event.h>
#include <uacpi/sleep.h>

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

void bootOtherProcessors() {
	uacpi_table madtTbl;

	auto ret = uacpi_table_find_by_signature("APIC", &madtTbl);
	assert(ret == UACPI_STATUS_OK);
	auto *madt = madtTbl.hdr;

	infoLogger() << "thor: Booting APs." << frg::endlog;

	size_t offset = sizeof(acpi_sdt_hdr) + sizeof(MadtHeader);
	while(offset < madt->length) {
		auto generic = (MadtGenericEntry *)(madtTbl.virt_addr + offset);
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
	uacpi_table madtTbl;

	auto ret = uacpi_table_find_by_signature("APIC", &madtTbl);
	assert(ret == UACPI_STATUS_OK);
	auto *madt = madtTbl.hdr;

	infoLogger() << "thor: Dumping MADT" << frg::endlog;

	size_t offset = sizeof(acpi_sdt_hdr) + sizeof(MadtHeader);
	while(offset < madt->length) {
		auto generic = (MadtGenericEntry *)(madtTbl.virt_addr + offset);
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

extern "C" EirInfo *thorBootInfoPtr;

initgraph::Stage *getTablesDiscoveredStage() {
	static initgraph::Stage s{&globalInitEngine, "acpi.tables-discovered"};
	return &s;
}

initgraph::Stage *getNsAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "acpi.ns-available"};
	return &s;
}

KernelFiber *acpiFiber;

static initgraph::Task initTablesTask{&globalInitEngine, "acpi.initialize",
	initgraph::Entails{getTablesDiscoveredStage()},
	[] {
		uacpi_init_params params = {
			.rsdp = thorBootInfoPtr->acpiRsdp,
			.log_level = UACPI_LOG_INFO,
			.flags = 0,
		};
		auto ret = uacpi_initialize(&params);
		assert(ret == UACPI_STATUS_OK);
	}
};

static initgraph::Task discoverIoApicsTask{&globalInitEngine, "acpi.discover-ioapics",
	initgraph::Requires{getTablesDiscoveredStage(),
		getFibersAvailableStage()},
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		dumpMadt();

		uacpi_table madtTbl;

		auto ret = uacpi_table_find_by_signature("APIC", &madtTbl);
		assert(ret == UACPI_STATUS_OK);
		auto *madt = madtTbl.hdr;

		// Configure all interrupt controllers.
		// TODO: This should be done during thor's initialization in order to avoid races.
		infoLogger() << "thor: Configuring I/O APICs." << frg::endlog;

		size_t offset = sizeof(acpi_sdt_hdr) + sizeof(MadtHeader);
		while(offset < madt->length) {
			auto generic = (MadtGenericEntry *)(madtTbl.virt_addr + offset);
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

		offset = sizeof(acpi_sdt_hdr) + sizeof(MadtHeader);
		while(offset < madt->length) {
			auto generic = (MadtGenericEntry *)(madtTbl.virt_addr + offset);
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

static initgraph::Task loadAcpiNamespaceTask{&globalInitEngine, "acpi.load-namespace",
	initgraph::Requires{getTaskingAvailableStage(),
		pci::getBus0AvailableStage()},
	initgraph::Entails{getNsAvailableStage()},
	[] {
		initGlue();

		auto ret = uacpi_namespace_load();
		assert(ret == UACPI_STATUS_OK);

		ret = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
		assert(ret == UACPI_STATUS_OK);

		initEc();

		ret = uacpi_namespace_initialize();
		assert(ret == UACPI_STATUS_OK);

		// Configure the ISA IRQs.
		// TODO: This is a hack. We assume that HPET will use legacy replacement.
		infoLogger() << "thor: Configuring ISA IRQs." << frg::endlog;
		configureIrq(resolveIsaIrq(0));
		configureIrq(resolveIsaIrq(1));
		configureIrq(resolveIsaIrq(4));
		configureIrq(resolveIsaIrq(12));
		configureIrq(resolveIsaIrq(14));

		initEvents();
	}
};

static initgraph::Task bootApsTask{&globalInitEngine, "acpi.boot-aps",
	initgraph::Requires{&loadAcpiNamespaceTask},
	[] {
		bootOtherProcessors();
	}
};

static initgraph::Task initPmInterfaceTask{&globalInitEngine, "acpi.init-pm-interface",
	initgraph::Requires{&loadAcpiNamespaceTask},
	[] {
		initializePmInterface();
	}
};

} } // namespace thor::acpi
