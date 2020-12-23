#pragma once

#include <stddef.h>
#include <stdint.h>

#include <frg/vector.hpp>
#include <frg/hash_map.hpp>
#include <thor-internal/framebuffer/fb.hpp>
#include <thor-internal/irq.hpp>

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

struct PciDevice;
struct PciBridge;
struct PciBus;
struct PciIrqRouter;

struct PciIrqRouter {
	PciIrqRouter(PciIrqRouter *parent_, PciBus *associatedBus_)
	: parent{parent_}, associatedBus{associatedBus_} { }

	virtual ~PciIrqRouter() {}

	IrqPin *resolveIrqRoute(uint32_t slot, IrqIndex index) {
		if (routingModel == RoutingModel::rootTable) {
			auto entry = std::find_if(routingTable.begin(), routingTable.end(),
					[&] (const auto &ref) { return ref.slot == slot && ref.index == index; });

			if(entry == routingTable.end())
				return nullptr;

			assert(entry->pin);
			return entry->pin;
		} else if(routingModel == RoutingModel::expansionBridge) {
			return bridgeIrqs[(static_cast<int>(index) - 1 + slot) % 4];
		} else {
			return nullptr;
		}
	}

	virtual PciIrqRouter *makeDownstreamRouter(PciBus *bus) = 0;

	PciIrqRouter *parent;
	PciBus *associatedBus;

protected:
	frg::vector<RoutingEntry, KernelAlloc> routingTable{*kernelAlloc};
	RoutingModel routingModel;
	IrqPin *bridgeIrqs[4] = {};
};

struct PciConfigIo;

struct PciBus {
	PciBus(PciBridge *associatedBridge_, PciIrqRouter *irqRouter_, PciConfigIo *io,
			uint32_t segId_, uint32_t busId_)
	: associatedBridge{associatedBridge_}, irqRouter{irqRouter_}, io{io},
		childDevices{*kernelAlloc}, childBridges{*kernelAlloc},
		segId{segId_}, busId{busId_} { }

	PciBus(const PciBus &) = delete;
	PciBus &operator=(const PciBus &) = delete;

	PciBus *makeDownstreamBus(PciBridge *bridge, uint32_t downstreamId) {
		auto newBus = frg::construct<PciBus>(*kernelAlloc, bridge, nullptr, io, segId, downstreamId);

		auto router = irqRouter->makeDownstreamRouter(newBus);
		newBus->irqRouter = router;

		return newBus;
	}

	PciBridge *associatedBridge;
	PciIrqRouter *irqRouter;
	PciConfigIo *io;
	frg::vector<PciDevice *, KernelAlloc> childDevices;
	frg::vector<PciBridge *, KernelAlloc> childBridges;

	uint32_t segId;
	uint32_t busId;
};

struct PciBar {
	enum BarType {
		kBarNone = 0,
		kBarIo = 1,
		kBarMemory = 2
	};

	PciBar()
	: type{kBarNone}, address{0}, length{0}, offset{0} { }

	BarType type;
	uintptr_t address;
	size_t length;

	bool allocated;
	smarter::shared_ptr<MemoryView> memory;
	smarter::shared_ptr<IoSpace> io;
	ptrdiff_t offset;
};

// Either a device or a bridge.
struct PciEntity {
	PciEntity(PciBus *parentBus_, uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function)
	: parentBus{parentBus_}, seg{seg}, bus{bus}, slot{slot}, function{function} { }

	PciEntity(const PciEntity &) = delete;

	PciEntity &operator= (const PciEntity &) = delete;

	PciBus *parentBus;

	virtual PciBar *getBars() = 0;

	// Location of the device on the PCI bus.
	uint32_t seg;
	uint32_t bus;
	uint32_t slot;
	uint32_t function;
};

struct PciBridge : PciEntity {
	PciBridge(PciBus *parentBus_, uint32_t seg, uint32_t bus,
			uint32_t slot, uint32_t function)
	: PciEntity{parentBus_, seg, bus, slot, function}, associatedBus{nullptr},
			downstreamId{0}, subordinateId{0} { }

	PciBar bars[2];

	PciBar *getBars() override {
		return bars;
	}

	PciBus *associatedBus;
	uint32_t downstreamId;
	uint32_t subordinateId;
};

struct PciDevice : PciEntity {
	struct Capability {
		unsigned int type;
		ptrdiff_t offset;
		size_t length;
	};

	PciDevice(PciBus *parentBus_, uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function,
			uint16_t vendor, uint16_t device_id, uint8_t revision,
			uint8_t class_code, uint8_t sub_class, uint8_t interface, uint16_t subsystem_vendor, uint16_t subsystem_device)
	: PciEntity{parentBus_, seg, bus, slot, function}, mbusId{0},
			vendor{vendor}, deviceId{device_id}, revision{revision},
			classCode{class_code}, subClass{sub_class}, interface{interface},
			subsystemVendor{subsystem_vendor}, subsystemDevice{subsystem_device},
			interrupt{nullptr}, caps{*kernelAlloc},
			associatedFrameBuffer{nullptr}, associatedScreen{nullptr} { }

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
	PciBar bars[6];

	PciBar *getBars() override {
		return bars;
	}

	frg::vector<Capability, KernelAlloc> caps;

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
	kPciBridgeSecondary = 0x19,
	kPciBridgeSubordinate = 0x1A
};

extern frg::manual_box<
	frg::vector<
		smarter::shared_ptr<PciDevice>,
		KernelAlloc
	>
> allDevices;

extern frg::manual_box<
	frg::vector<
		PciBus *,
		KernelAlloc
	>
> allRootBuses;

extern frg::manual_box<
	frg::hash_map<
		uint32_t,
		PciConfigIo *,
		frg::hash<uint32_t>,
		KernelAlloc
	>
> allConfigSpaces;

void runAllDevices();

void addToEnumerationQueue(PciBus *bus);
void addRootBus(PciBus *bus);
void enumerateAll();

struct PciConfigIo {
	uint8_t readConfigByte(PciBus *bus, uint32_t slot, uint32_t function, uint16_t offset) {
		return readConfigByte(bus->segId, bus->busId, slot, function, offset);
	}

	uint16_t readConfigHalf(PciBus *bus, uint32_t slot, uint32_t function, uint16_t offset) {
		return readConfigHalf(bus->segId, bus->busId, slot, function, offset);
	}

	uint32_t readConfigWord(PciBus *bus, uint32_t slot, uint32_t function, uint16_t offset) {
		return readConfigWord(bus->segId, bus->busId, slot, function, offset);
	}

	void writeConfigByte(PciBus *bus, uint32_t slot, uint32_t function, uint16_t offset, uint8_t value) {
		writeConfigByte(bus->segId, bus->busId, slot, function, offset, value);
	}

	void writeConfigHalf(PciBus *bus, uint32_t slot, uint32_t function, uint16_t offset, uint16_t value) {
		writeConfigHalf(bus->segId, bus->busId, slot, function, offset, value);
	}

	void writeConfigWord(PciBus *bus, uint32_t slot, uint32_t function, uint16_t offset, uint32_t value) {
		writeConfigWord(bus->segId, bus->busId, slot, function, offset, value);
	}

	virtual uint8_t readConfigByte(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset) = 0;
	virtual uint16_t readConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset) = 0;
	virtual uint32_t readConfigWord(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset) = 0;

	virtual void writeConfigByte(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset, uint8_t value) = 0;
	virtual void writeConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset, uint16_t value) = 0;
	virtual void writeConfigWord(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset, uint32_t value) = 0;
};

void addConfigSpaceIo(uint32_t seg, uint32_t bus, PciConfigIo *io);

inline bool isValidConfigAccess(int size, uint32_t offset) {
	assert(size == 1 || size == 2 || size == 4);
	return !(offset & (size - 1));
}

inline PciConfigIo *getConfigIoFor(uint32_t seg, uint32_t bus) {
	return (*allConfigSpaces)[(seg << 8) | bus];
}

// read from pci configuration space
uint32_t readConfigWord(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset);
uint16_t readConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset);
uint8_t readConfigByte(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset);

// write to pci configuration space
void writeConfigWord(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset, uint32_t value);
void writeConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset, uint16_t value);
void writeConfigByte(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset, uint8_t value);
} } // namespace thor::pci
