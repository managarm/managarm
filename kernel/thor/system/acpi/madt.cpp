#include <algorithm>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/optional.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include "../../arch/x86/cpu.hpp"
#include "../../arch/x86/hpet.hpp"
#include "../../arch/x86/pic.hpp"
#include "../../generic/kernel_heap.hpp"
#include "../../system/pci/pci.hpp"
#include "pm-interface.hpp"

extern "C" {
#include <acpi.h>
}

namespace thor {
namespace acpi {

void acpicaCheckFailed(const char *expr, const char *file, int line) {
	frigg::panicLogger() << "ACPICA_CHECK failed: "
			<< expr << "\nIn file " << file << " on line " << line
			<< frigg::endLog;
}

#define ACPICA_CHECK(expr) do { if((expr) != AE_OK) { \
		acpicaCheckFailed(#expr, __FILE__, __LINE__); } } while(0)

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
	ACPI_GENERIC_ADDRESS address;
	uint8_t hpetNumber;
	uint16_t minimumTick;
	uint8_t pageProtection;
} __attribute__ (( packed ));

// --------------------------------------------------------

struct ScopedBuffer {
	friend void swap(ScopedBuffer &a, ScopedBuffer &b) {
		using std::swap;
		swap(a._object, b._object);
	}

	ScopedBuffer() {
		_object.Length = ACPI_ALLOCATE_BUFFER;
		_object.Pointer = nullptr;
	}

	ScopedBuffer(const ScopedBuffer &) = delete;
	
	ScopedBuffer(ScopedBuffer &&other)
	: ScopedBuffer() {
		swap(*this, other);
	}


	~ScopedBuffer() {
		if(_object.Pointer)
			AcpiOsFree(_object.Pointer);
	}

	ScopedBuffer &operator= (ScopedBuffer other) {
		swap(*this, other);
		return *this;
	}

	size_t size() {
		assert(_object.Pointer);
		return _object.Length;
	}

	void *data() {
		assert(_object.Pointer);
		return _object.Pointer;
	}

	ACPI_BUFFER *get() {
		return &_object;
	}

private:
	ACPI_BUFFER _object;
};

frigg::Vector<ACPI_HANDLE, KernelAlloc> getChildren(ACPI_HANDLE parent) {
	frigg::Vector<ACPI_HANDLE, KernelAlloc> results{*kernelAlloc};
	ACPI_HANDLE child = nullptr;
	while(true) {
		ACPI_STATUS status = AcpiGetNextObject(ACPI_TYPE_ANY, parent, child, &child);
		if(status == AE_NOT_FOUND)
			break;
		ACPICA_CHECK(status);
		
		results.push(child);
	}
	return results;
}

bool hasChild(ACPI_HANDLE parent, const char *path) {
	ACPI_HANDLE child = nullptr;
	while(true) {
		ACPI_STATUS status = AcpiGetNextObject(ACPI_TYPE_ANY, parent, child, &child);
		if(status == AE_NOT_FOUND)
			return false;
		ACPICA_CHECK(status);
	
		acpi::ScopedBuffer buffer;
		ACPICA_CHECK(AcpiGetName(child, ACPI_SINGLE_NAME, buffer.get()));
		if(!strcmp(static_cast<char *>(buffer.data()), path))
			return true;
	}
}

ACPI_HANDLE getChild(ACPI_HANDLE parent, const char *path) {
	ACPI_HANDLE child;
	ACPICA_CHECK(AcpiGetHandle(parent, const_cast<char *>(path), &child));
	return child;
}

uint64_t evaluate(ACPI_HANDLE handle) {
	acpi::ScopedBuffer buffer;
	ACPICA_CHECK(AcpiEvaluateObject(handle, nullptr, nullptr, buffer.get()));
	auto object = reinterpret_cast<ACPI_OBJECT *>(buffer.data());
	assert(object->Type == ACPI_TYPE_INTEGER);
	return object->Integer.Value;
}

void evaluateWith1(ACPI_HANDLE handle) {
	ACPI_OBJECT args[1];
	args[0].Integer.Type = ACPI_TYPE_INTEGER;
	args[0].Integer.Value = 1;

	ACPI_OBJECT_LIST list;
	list.Count = 1;
	list.Pointer = args;

	ACPICA_CHECK(AcpiEvaluateObject(handle, nullptr, &list, nullptr));
}

template<typename F>
void walkResources(ACPI_HANDLE object, const char *method, F functor) {
	auto fptr = [] (ACPI_RESOURCE *r, void *c) -> ACPI_STATUS {
		(*static_cast<F *>(c))(r);
		return AE_OK;
	};
	ACPICA_CHECK(AcpiWalkResources(object, const_cast<char *>(method), fptr, &functor));
}

// --------------------------------------------------------

struct IrqConfiguration {
	bool specified() {
		return trigger != TriggerMode::null
				&& polarity != Polarity::null;
	}

	bool compatible(IrqConfiguration other) {
		assert(specified());
		return trigger == other.trigger
				&& polarity == other.polarity;
	}

	TriggerMode trigger;
	Polarity polarity;
};

// Stores the IRQ override information (GSI, trigger mode, polarity)
// of a bus-specific IRQ (e.g., on the ISA bus).
struct IrqOverride {
	unsigned int gsi;
	IrqConfiguration configuration;
};

IrqOverride defaultIrq(unsigned int irq) {
	return IrqOverride{irq, IrqConfiguration{TriggerMode::edge, Polarity::high}};
}

frigg::LazyInitializer<frigg::Optional<IrqOverride>> irqOverrides[16];

IrqOverride resolveIsaIrq(unsigned int irq) {
	assert(irq < 16);
	if((*irqOverrides[irq]))
		return *(*irqOverrides[irq]);
	return defaultIrq(irq);
}

// Same as resolveIsaIrq(irq) but allows to set more specific configuration options.
IrqOverride resolveIsaIrq(unsigned int irq, IrqConfiguration desired) {
	if(irq < 16 && *irqOverrides[irq]) {
		assert(desired.compatible((*irqOverrides[irq])->configuration));
		return *(*irqOverrides[irq]);
	}
	return IrqOverride{irq, desired};
}

// --------------------------------------------------------

void commitIrq(IrqOverride line) {
	auto pin = getGlobalSystemIrq(line.gsi);
	pin->configure(line.configuration.trigger, line.configuration.polarity);
}

// --------------------------------------------------------

uint32_t handlePowerButton(void *context) {
	frigg::infoLogger() << "thor: Preparing for shutdown" << frigg::endLog;

	ACPICA_CHECK(AcpiEnterSleepStatePrep(5));
	ACPICA_CHECK(AcpiEnterSleepState(5));

	return ACPI_INTERRUPT_HANDLED;
}

void dispatchEvent(uint32_t type, ACPI_HANDLE device, uint32_t number, void *context) {
	if(type == ACPI_EVENT_TYPE_FIXED) {
		frigg::infoLogger() << "thor: Fixed ACPI event" << frigg::endLog;
	}else{
		assert(type == ACPI_EVENT_TYPE_GPE);
		frigg::infoLogger() << "thor: ACPI GPE event" << frigg::endLog;
	}
}

// --------------------------------------------------------

frigg::String<KernelAlloc> getHardwareId(ACPI_HANDLE handle) {
	frigg::String<KernelAlloc> string{*kernelAlloc};

	ACPI_DEVICE_INFO *info;
	ACPICA_CHECK(AcpiGetObjectInfo(handle, &info));
	if(info->HardwareId.Length)
		string = frigg::String<KernelAlloc>{*kernelAlloc,
				info->HardwareId.String, info->HardwareId.Length - 1};
	ACPI_FREE(info);

	return string;
}

// --------------------------------------------------------

IrqConfiguration irqConfig[256];

void configureIrq(unsigned int gsi, IrqConfiguration desired) {
	assert(gsi < 256);
	assert(desired.specified());
	if(!irqConfig[gsi].specified()) {
		auto pin = getGlobalSystemIrq(gsi);
		pin->configure(desired.trigger, desired.polarity);
		irqConfig[gsi] = desired;
	}else{
		assert(irqConfig[gsi].compatible(desired));
	}
}

IrqPin *configureRoute(const char *link_path) {
	auto decodeTrigger = [] (unsigned int trigger) {
		switch(trigger) {
		case ACPI_LEVEL_SENSITIVE: return TriggerMode::level;
		case ACPI_EDGE_SENSITIVE: return TriggerMode::edge;
		default: frigg::panicLogger() << "Bad ACPI IRQ trigger mode" << frigg::endLog;
		}
	};
	auto decodePolarity = [] (unsigned int polarity) {
		switch(polarity) {
		case ACPI_ACTIVE_HIGH: return Polarity::high;
		case ACPI_ACTIVE_LOW: return Polarity::low;
		default: frigg::panicLogger() << "Bad ACPI IRQ polarity" << frigg::endLog;
		}
	};
	
	// TODO: Hack to null-terminate the string.
	auto handle = getChild(ACPI_ROOT_OBJECT, link_path);

	if(hasChild(handle, "_STA")) {
		auto status = evaluate(getChild(handle, "_STA"));
		if(!(status & 1)) {
			frigg::infoLogger() << "    Link device is not present." << frigg::endLog;
			return nullptr;
		}else if(!(status & 2)) {
			frigg::infoLogger() << "    Link device is not enabled." << frigg::endLog;
			return nullptr;
		}
	}

	IrqPin *pin = nullptr;
	walkResources(handle, "_CRS", [&] (ACPI_RESOURCE *r) {
		if(r->Type == ACPI_RESOURCE_TYPE_IRQ) {
			assert(r->Data.Irq.InterruptCount == 1);
			auto trigger = decodeTrigger(r->Data.ExtendedIrq.Triggering);
			auto polarity = decodePolarity(r->Data.ExtendedIrq.Polarity);
			frigg::infoLogger() << "    Resource: Irq "
					<< (int)r->Data.Irq.Interrupts[0]
					<< ", trigger mode: " << static_cast<int>(trigger)
					<< ", polarity: " << static_cast<int>(polarity)
					<< frigg::endLog;
			assert(!pin);
			configureIrq(r->Data.Irq.Interrupts[0], {trigger, polarity});
			pin = getGlobalSystemIrq(r->Data.Irq.Interrupts[0]);
		}else if(r->Type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
			assert(r->Data.ExtendedIrq.InterruptCount == 1);
			auto trigger = decodeTrigger(r->Data.ExtendedIrq.Triggering);
			auto polarity = decodePolarity(r->Data.ExtendedIrq.Polarity);
			frigg::infoLogger() << "    Resource: Extended Irq "
					<< (int)r->Data.ExtendedIrq.Interrupts[0]
					<< ", trigger mode: " << static_cast<int>(trigger)
					<< ", polarity: " << static_cast<int>(polarity)
					<< frigg::endLog;
			assert(!pin);
			configureIrq(r->Data.ExtendedIrq.Interrupts[0], {trigger, polarity});
			pin = getGlobalSystemIrq(r->Data.ExtendedIrq.Interrupts[0]);
		}else if(r->Type != ACPI_RESOURCE_TYPE_END_TAG) {
			frigg::infoLogger() << "    Resource: [Type "
					<< r->Type << "]" << frigg::endLog;
		}
	});

	assert(pin);
	return pin;
}

void enumerateSystemBusses() {
	auto sb = getChild(ACPI_ROOT_OBJECT, "_SB_");
	for(auto child : getChildren(sb)) {
		auto id = getHardwareId(child);
		if(id != "PNP0A03" && id != "PNP0A08")
			continue;
		
		frigg::infoLogger() << "thor: Found PCI host bridge" << frigg::endLog;

		pci::RoutingInfo routing{*kernelAlloc};

		acpi::ScopedBuffer buffer;
		ACPICA_CHECK(AcpiGetIrqRoutingTable(child, buffer.get()));

		size_t offset = 0;
		while(true) {
			auto route = (ACPI_PCI_ROUTING_TABLE *)((char *)buffer.data() + offset);
			if(!route->Length)
				break;
			
			auto slot = route->Address >> 16;
			auto function = route->Address & 0xFFFF;
			assert(function == 0xFFFF);
			auto index = static_cast<pci::IrqIndex>(route->Pin + 1);
			if(!*route->Source) {
				frigg::infoLogger() << "    Route for slot " << slot
						<< ", " << nameOf(index) << ": "
						<< "GSI " << route->SourceIndex << frigg::endLog;

				configureIrq(route->SourceIndex, {TriggerMode::level, Polarity::low});
				auto pin = getGlobalSystemIrq(route->SourceIndex);
				routing.push({slot, index, pin});
			}else{
				frigg::infoLogger() << "    Route for slot " << slot
						<< ", " << nameOf(index) << ": " << (const char *)route->Source
						<< "[" << route->SourceIndex << "]" << frigg::endLog;

				assert(!route->SourceIndex);
				auto pin = configureRoute(const_cast<const char *>(route->Source));
				routing.push({slot, index, pin});
			}

			offset += route->Length;
		}

		pci::pciDiscover(routing);
	}
}

// --------------------------------------------------------

void bootOtherProcessors() {
	ACPI_TABLE_HEADER *madt;
	ACPICA_CHECK(AcpiGetTable(const_cast<char *>("APIC"), 0, &madt));

	frigg::infoLogger() << "thor: Booting APs." << frigg::endLog;
	
	size_t offset = sizeof(ACPI_TABLE_HEADER) + sizeof(MadtHeader);
	while(offset < madt->Length) {
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
	ACPI_TABLE_HEADER *madt;
	ACPICA_CHECK(AcpiGetTable(const_cast<char *>("APIC"), 0, &madt));

	frigg::infoLogger() << "thor: Dumping MADT" << frigg::endLog;

	size_t offset = sizeof(ACPI_TABLE_HEADER) + sizeof(MadtHeader);
	while(offset < madt->Length) {
		auto generic = (MadtGenericEntry *)((uintptr_t)madt + offset);
		if(generic->type == 0) { // local APIC
			auto entry = (MadtLocalEntry *)generic;
			frigg::infoLogger() << "    Local APIC id: "
					<< (int)entry->localApicId
					<< ((entry->flags & local_flags::enabled) ? "" :" (disabled)")
					<< frigg::endLog;

			// TODO: This has to be refactored.
//			uint32_t id = entry->localApicId;
//			if(seen_bsp)
//				helControlKernel(kThorSubArch, kThorIfBootSecondary,
//						&id, nullptr);
//			seen_bsp = 1;
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

void initializeBasicSystem() {
	ACPICA_CHECK(AcpiInitializeSubsystem());
	ACPICA_CHECK(AcpiInitializeTables(nullptr, 16, FALSE));
	ACPICA_CHECK(AcpiLoadTables());

	frigg::infoLogger() << "thor: ACPICA initialized." << frigg::endLog;

	dumpMadt();
	
	ACPI_TABLE_HEADER *madt;
	ACPICA_CHECK(AcpiGetTable(const_cast<char *>("APIC"), 0, &madt));

	// Configure all interrupt controllers.
	// TODO: This should be done during thor's initialization in order to avoid races.
	frigg::infoLogger() << "thor: Configuring I/O APICs." << frigg::endLog;
	
	size_t offset = sizeof(ACPI_TABLE_HEADER) + sizeof(MadtHeader);
	while(offset < madt->Length) {
		auto generic = (MadtGenericEntry *)((uint8_t *)madt + offset);
		if(generic->type == 1) { // I/O APIC
			auto entry = (MadtIoEntry *)generic;
			setupIoApic(entry->ioApicId, entry->systemIntBase, entry->mmioAddress);
		}
		offset += generic->length;
	}
	
	// Determine IRQ override configuration.
	for(int i = 0; i < 16; i++)
		irqOverrides[i].initialize();

	offset = sizeof(ACPI_TABLE_HEADER) + sizeof(MadtHeader);
	while(offset < madt->Length) {
		auto generic = (MadtGenericEntry *)((uint8_t *)madt + offset);
		if(generic->type == 2) { // interrupt source override
			auto entry = (MadtIntOverrideEntry *)generic;
			
			// ACPI defines only ISA IRQ overrides.
			assert(entry->bus == 0);
			assert(entry->sourceIrq < 16);

			IrqOverride line;
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

			assert(!(*irqOverrides[entry->sourceIrq]));
			*irqOverrides[entry->sourceIrq] = line;
		}
		offset += generic->length;
	}
	
	// Initialize the HPET.
	frigg::infoLogger() << "thor: Setting up HPET." << frigg::endLog;
	ACPI_TABLE_HEADER *hpet_table;
	ACPICA_CHECK(AcpiGetTable(const_cast<char *>("HPET"), 0, &hpet_table));

	auto hpet_entry = (HpetEntry *)((uintptr_t)hpet_table + sizeof(ACPI_TABLE_HEADER));
	assert(hpet_entry->address.SpaceId == ACPI_ADR_SPACE_SYSTEM_MEMORY);
	setupHpet(hpet_entry->address.Address);
}

void initializeExtendedSystem() {
	// Configure the ISA IRQs.
	// TODO: This is a hack. We assume that HPET will use legacy replacement
	// and that SCI is routed to IRQ 9.
	frigg::infoLogger() << "thor: Configuring ISA IRQs." << frigg::endLog;
	commitIrq(resolveIsaIrq(0));
	commitIrq(resolveIsaIrq(1));
	commitIrq(resolveIsaIrq(4));
	commitIrq(resolveIsaIrq(9));
	commitIrq(resolveIsaIrq(12));
	commitIrq(resolveIsaIrq(14));
	
	frigg::infoLogger() << "thor: Entering ACPI mode." << frigg::endLog;
	ACPICA_CHECK(AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION));

	ACPICA_CHECK(AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON,
			&handlePowerButton, nullptr));
	ACPICA_CHECK(AcpiInstallGlobalEventHandler(&dispatchEvent, nullptr));

	ACPICA_CHECK(AcpiInitializeObjects(ACPI_FULL_INITIALIZATION));
	
	if(hasChild(ACPI_ROOT_OBJECT, "_PIC")) {
		frigg::infoLogger() << "thor: Invoking \\_PIC method" << frigg::endLog;
		evaluateWith1(getChild(ACPI_ROOT_OBJECT, "_PIC"));
	}

	bootOtherProcessors();
	enumerateSystemBusses();

	initializePmInterface();
	
	frigg::infoLogger() << "thor: System configuration complete." << frigg::endLog;
}

} } // namespace thor::acpi
