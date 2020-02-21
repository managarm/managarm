#ifndef THOR_SYSTEM_PCI_PCI_HPP
#define THOR_SYSTEM_PCI_PCI_HPP

#include <stddef.h>
#include <stdint.h>
#include <frigg/smart_ptr.hpp>
#include <frigg/vector.hpp>
#include "../fb.hpp"
#include "../../generic/irq.hpp"

#include <lai/core.h>

namespace thor {

struct MemoryView;
struct IoSpace;

struct BootScreen;

namespace pci {

enum class IrqIndex {
	null, inta, intb, intc, intd
};

inline const char *nameOf(IrqIndex index) {
	switch(index) {
	case IrqIndex::inta: return "INTA";
	case IrqIndex::intb: return "INTB";
	case IrqIndex::intc: return "INTC";
	case IrqIndex::intd: return "INTD";
	default:
		assert(!"Illegal PCI interrupt pin for nameOf(IrqIndex)");
		__builtin_unreachable();
	}
}

inline const char *nameOfCapability(unsigned int type) {
	switch(type) {
	case 0x04: return "Slot-identification";
	case 0x05: return "MSI";
	case 0x09: return "Vendor-specific";
	case 0x0A: return "Debug-port";
	case 0x10: return "PCIe";
	case 0x11: return "MSI-X";
	default:
		return nullptr;
	}
}

enum class RoutingModel {
	none,
	rootTable, // Routing table of PCI IRQ pins to global IRQs (i.e., PRT).
	expansionBridge // Default routing of expansion bridges.
};

struct RoutingEntry {
	unsigned int slot;
	IrqIndex index;
	IrqPin *pin;
};

struct PciBridge;
struct PciBus;

struct PciBus {
	PciBus(PciBridge *associatedBridge_, uint32_t busId_, lai_nsnode_t *acpiHandle_);

	PciBus(const PciBus &) = delete;

	PciBus &operator=(const PciBus &) = delete;

	IrqPin *resolveIrqRoute(unsigned int slot, IrqIndex index);

	PciBridge *associatedBridge;

	uint32_t busId;

	lai_nsnode_t *acpiHandle = nullptr;

private:
	RoutingModel _routingModel = RoutingModel::none;
	// PRT of this bus (RoutingModel::rootTable).
	frigg::Vector<RoutingEntry, KernelAlloc> _routingTable{*kernelAlloc};
	// IRQs of the bridge (RoutingModel::expansionBridge).
	IrqPin *_bridgeIrqs[4] = {};
};

// Either a device or a bridge.
struct PciEntity {
	PciEntity(PciBus *parentBus_, uint32_t bus, uint32_t slot, uint32_t function)
	: parentBus{parentBus_}, bus{bus}, slot{slot}, function{function} { }

	PciEntity(const PciEntity &) = delete;

	PciEntity &operator= (const PciEntity &) = delete;

	PciBus *parentBus;

	// Location of the device on the PCI bus.
	uint32_t bus;
	uint32_t slot;
	uint32_t function;
};

struct PciBridge : PciEntity {
	PciBridge(PciBus *parentBus_, uint32_t bus, uint32_t slot, uint32_t function)
	: PciEntity{parentBus_, bus, slot, function} { }
};

struct PciDevice : PciEntity {
	enum BarType {
		kBarNone = 0,
		kBarIo = 1,
		kBarMemory = 2
	};

	struct Bar {
		Bar()
		: type(kBarNone), address(0), length(0), offset(0) { }

		BarType type;
		uintptr_t address;
		size_t length;
		
		frigg::SharedPtr<MemoryView> memory;
		frigg::SharedPtr<IoSpace> io;
		ptrdiff_t offset;
	};

	struct Capability {
		unsigned int type;
		ptrdiff_t offset;
		size_t length;
	};

	PciDevice(PciBus *parentBus_, uint32_t bus, uint32_t slot, uint32_t function,
			uint32_t vendor, uint32_t device_id, uint8_t revision,
			uint8_t class_code, uint8_t sub_class, uint8_t interface, uint16_t subsystem_vendor, uint16_t subsystem_device)
	: PciEntity{parentBus_, bus, slot, function}, mbusId(0),
			vendor(vendor), deviceId(device_id), revision(revision),
			classCode(class_code), subClass(sub_class), interface(interface), subsystemVendor(subsystem_vendor), subsystemDevice(subsystem_device),
			interrupt(nullptr), caps(*kernelAlloc),
			associatedFrameBuffer(nullptr), associatedScreen(nullptr) { }
	
	// mbus object ID of the device
	int64_t mbusId;

	// vendor-specific device information
	uint16_t vendor;
	uint16_t deviceId;
	uint8_t revision;

	// generic device information
	uint8_t classCode;
	uint8_t subClass;
	uint8_t interface;

	uint16_t subsystemVendor;
	uint16_t subsystemDevice;

	IrqPin *interrupt;
	
	// device configuration
	Bar bars[6];

	frigg::Vector<Capability, KernelAlloc> caps;

	// Device attachments.
	FbInfo *associatedFrameBuffer;
	BootScreen *associatedScreen;
};

enum {
	// general PCI header fields
	kPciVendor = 0,
	kPciDevice = 2,
	kPciCommand = 4,
	kPciStatus = 6,
	kPciRevision = 0x08,
	kPciInterface = 0x09,
	kPciSubClass = 0x0A,
	kPciClassCode = 0x0B,
	kPciHeaderType = 0x0E,

	// usual device header fields
	kPciRegularBar0 = 0x10,
	kPciRegularSubsystemVendor = 0x2C,
	kPciRegularSubsystemDevice = 0x2E,
	kPciRegularCapabilities = 0x34,
	kPciRegularInterruptLine = 0x3C,
	kPciRegularInterruptPin = 0x3D,

	// PCI-to-PCI bridge header fields
	kPciBridgeSecondary = 0x19
};

extern frigg::LazyInitializer<frigg::Vector<frigg::SharedPtr<PciDevice>, KernelAlloc>> allDevices;

void enumerateSystemBusses();

void runAllDevices();

} } // namespace thor::pci

// read from pci configuration space
uint32_t readPciWord(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset);
uint16_t readPciHalf(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset);
uint8_t readPciByte(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset);

// write to pci configuration space
void writePciWord(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset, uint32_t value);
void writePciHalf(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset, uint16_t value);
void writePciByte(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset, uint8_t value);

#endif // THOR_SYSTEM_PCI_PCI_HPP
