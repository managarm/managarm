#include <algorithm>
#include <frg/optional.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/hpet.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/kernel_heap.hpp>
#include "../../system/pci/pci.hpp"
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/acpi/pm-interface.hpp>

#include <lai/core.h>
#include <lai/helpers/pc-bios.h>
#include <lai/helpers/pci.h>
#include <lai/helpers/pm.h>
#include <lai/helpers/sci.h>

namespace thor {
namespace acpi {

struct MadtHeader {
	uint32_t localApicAddress;
	uint32_t flags;
};

struct MadtGenericEntry {
	uint8_t type;
	uint8_t length;
};

struct MadtLocalEntry {
	MadtGenericEntry generic;
	uint8_t processorId;
	uint8_t localApicId;
	uint32_t flags;
};

namespace local_flags {
	static constexpr uint32_t enabled = 1;
};

struct MadtIoEntry {
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

struct MadtIntOverrideEntry {
	MadtGenericEntry generic;
	uint8_t bus;
	uint8_t sourceIrq;
	uint32_t systemInt;
	uint16_t flags;
};

struct MadtLocalNmiEntry {
	MadtGenericEntry generic;
	uint8_t processorId;
	uint16_t flags;
	uint8_t localInt;
} __attribute__ (( packed ));

struct HpetEntry {
	uint32_t generalCapsAndId;
	acpi_gas_t address;
	uint8_t hpetNumber;
	uint16_t minimumTick;
	uint8_t pageProtection;
} __attribute__ (( packed ));

} } // namespace thor::acpi

// --------------------------------------------------------

namespace thor {

frigg::LazyInitializer<frg::optional<GlobalIrqInfo>> isaIrqOverrides[16];

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
	auto pin = getGlobalSystemIrq(info.gsi);
	assert(pin);
	pin->configure(info.configuration);
}

} // namespace thor

// --------------------------------------------------------

namespace thor {
namespace acpi {

struct SciDevice : IrqSink {
	SciDevice()
	: IrqSink{frigg::String<KernelAlloc>{*kernelAlloc, "acpi-sci"}} { }

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

frigg::LazyInitializer<SciDevice> sciDevice;

// --------------------------------------------------------

void bootOtherProcessors() {
	void *madtWindow = laihost_scan("APIC", 0);
	assert(madtWindow);
	auto madt = reinterpret_cast<acpi_header_t *>(madtWindow);

	frigg::infoLogger() << "thor: Booting APs." << frigg::endLog;

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

	frigg::infoLogger() << "thor: Dumping MADT" << frigg::endLog;

	size_t offset = sizeof(acpi_header_t) + sizeof(MadtHeader);
	while(offset < madt->length) {
		auto generic = (MadtGenericEntry *)((uintptr_t)madt + offset);
		if(generic->type == 0) { // local APIC
			auto entry = (MadtLocalEntry *)generic;
			frigg::infoLogger() << "    Local APIC id: "
					<< (int)entry->localApicId
					<< ((entry->flags & local_flags::enabled) ? "" :" (disabled)")
					<< frigg::endLog;
		}else if(generic->type == 1) { // I/O APIC
			auto entry = (MadtIoEntry *)generic;
			frigg::infoLogger() << "    I/O APIC id: " << (int)entry->ioApicId
					<< ", sytem interrupt base: " << (int)entry->systemIntBase
					<< frigg::endLog;
		}else if(generic->type == 2) { // interrupt source override
			auto entry = (MadtIntOverrideEntry *)generic;

			const char *bus, *polarity, *trigger;
			if(entry->bus == 0) {
				bus = "ISA";
			}else{
				frigg::panicLogger() << "Unexpected bus in MADT interrupt override"
						<< frigg::endLog;
			}

			if((entry->flags & OverrideFlags::polarityMask) == OverrideFlags::polarityDefault) {
				polarity = "default";
			}else if((entry->flags & OverrideFlags::polarityMask) == OverrideFlags::polarityHigh) {
				polarity = "high";
			}else if((entry->flags & OverrideFlags::polarityMask) == OverrideFlags::polarityLow) {
				polarity = "low";
			}else{
				frigg::panicLogger() << "Unexpected polarity in MADT interrupt override"
						<< frigg::endLog;
			}

			if((entry->flags & OverrideFlags::triggerMask) == OverrideFlags::triggerDefault) {
				trigger = "default";
			}else if((entry->flags & OverrideFlags::triggerMask) == OverrideFlags::triggerEdge) {
				trigger = "edge";
			}else if((entry->flags & OverrideFlags::triggerMask) == OverrideFlags::triggerLevel) {
				trigger = "level";
			}else{
				frigg::panicLogger() << "Unexpected trigger mode in MADT interrupt override"
						<< frigg::endLog;
			}

			frigg::infoLogger() << "    Int override: " << bus << " IRQ " << (int)entry->sourceIrq
					<< " is mapped to GSI " << entry->systemInt
					<< " (Polarity: " << polarity << ", trigger mode: " << trigger
					<< ")" << frigg::endLog;
		}else if(generic->type == 4) { // local APIC NMI source
			auto entry = (MadtLocalNmiEntry *)generic;
			frigg::infoLogger() << "    Local APIC NMI: processor " << (int)entry->processorId
					<< ", lint: " << (int)entry->localInt << frigg::endLog;
		}else{
			frigg::infoLogger() << "    Unexpected MADT entry of type "
					<< generic->type << frigg::endLog;
		}
		offset += generic->length;
	}
}

void *globalRsdtWindow;
int globalRsdtVersion;

void initializeBasicSystem() {
	lai_rsdp_info rsdp_info;
	if(lai_bios_detect_rsdp(&rsdp_info))
		frigg::panicLogger() << "thor: Could not detect ACPI" << frigg::endLog;

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

	dumpMadt();

	void *madtWindow = laihost_scan("APIC", 0);
	assert(madtWindow);
	auto madt = reinterpret_cast<acpi_header_t *>(madtWindow);

	// Configure all interrupt controllers.
	// TODO: This should be done during thor's initialization in order to avoid races.
	frigg::infoLogger() << "thor: Configuring I/O APICs." << frigg::endLog;

	size_t offset = sizeof(acpi_header_t) + sizeof(MadtHeader);
	while(offset < madt->length) {
		auto generic = (MadtGenericEntry *)((uint8_t *)madtWindow + offset);
		if(generic->type == 1) { // I/O APIC
			auto entry = (MadtIoEntry *)generic;
			setupIoApic(entry->ioApicId, entry->systemIntBase, entry->mmioAddress);
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
					frigg::panicLogger() << "Illegal IRQ trigger mode in MADT" << frigg::endLog;
				}

				switch(polarity) {
				case OverrideFlags::polarityHigh:
					line.configuration.polarity = Polarity::high; break;
				case OverrideFlags::polarityLow:
					line.configuration.polarity = Polarity::low; break;
				default:
					frigg::panicLogger() << "Illegal IRQ polarity in MADT" << frigg::endLog;
				}
			}

			assert(!(*isaIrqOverrides[entry->sourceIrq]));
			*isaIrqOverrides[entry->sourceIrq] = line;
		}
		offset += generic->length;
	}

	// Initialize the HPET.
	[&] () {
		void *hpetWindow = laihost_scan("HPET", 0);
		if(!hpetWindow) {
			frigg::infoLogger() << "\e[31m" "thor: No HPET table!" "\e[39m" << frigg::endLog;
			return;
		}
		auto hpet = reinterpret_cast<acpi_header_t *>(hpetWindow);
		if(hpet->length < sizeof(acpi_header_t) + sizeof(HpetEntry)) {
			frigg::infoLogger() << "\e[31m" "thor: HPET table has no entries!" "\e[39m"
					<< frigg::endLog;
			return;
		}
		auto hpetEntry = (HpetEntry *)((uintptr_t)hpetWindow + sizeof(acpi_header_t));
		frigg::infoLogger() << "thor: Setting up HPET" << frigg::endLog;
		assert(hpetEntry->address.address_space == ACPI_GAS_MMIO);
		setupHpet(hpetEntry->address.base);
	}();
}

void initializeExtendedSystem() {
	// Configure the ISA IRQs.
	// TODO: This is a hack. We assume that HPET will use legacy replacement.
	frigg::infoLogger() << "thor: Configuring ISA IRQs." << frigg::endLog;
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
	IrqPin::attachSink(getGlobalSystemIrq(sciOverride.gsi), sciDevice.get());

	// Enable ACPI.
	frigg::infoLogger() << "thor: Entering ACPI mode." << frigg::endLog;
	lai_enable_acpi(1);

	bootOtherProcessors();
	pci::enumerateSystemBusses();
	initializePmInterface();

	frigg::infoLogger() << "thor: System configuration complete." << frigg::endLog;
}

} } // namespace thor::acpi
