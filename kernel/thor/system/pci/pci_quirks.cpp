#include <frg/optional.hpp>

#include <thor-internal/debug.hpp>
#include <thor-internal/pci/pci.hpp>

namespace thor::pci {

void uploadRaspberryPi4Vl805Firmware(PciDevice *dev);

namespace {

void uhciSmiDisable(PciDevice *dev) {
	debugLogger() << "            Disabling UHCI SMI generation!" << frg::endlog;
	dev->parentBus->io->writeConfigHalf(dev->parentBus, dev->slot, dev->function, 0xC0, 0x2000);
}

void switchUsbPortsToXhci(PciDevice *dev) {
	debugLogger() << "            Switching USB ports to XHCI!" << frg::endlog;
	auto io = dev->parentBus->io;

	auto usb3PortsAvail = io->readConfigWord(dev->parentBus, dev->slot, dev->function, 0xDC);
	io->writeConfigWord(dev->parentBus, dev->slot, dev->function, 0xD8, usb3PortsAvail);

	auto usb2PortsAvail = io->readConfigWord(dev->parentBus, dev->slot, dev->function, 0xD4);
	io->writeConfigWord(dev->parentBus, dev->slot, dev->function, 0xD0, usb2PortsAvail);
}

struct {
	std::optional<uint8_t> pci_class = std::nullopt;
	std::optional<uint8_t> pci_subclass = std::nullopt;
	std::optional<uint8_t> pci_interface = std::nullopt;
	std::optional<uint16_t> pci_vendor = std::nullopt;
	std::optional<uint16_t> pci_segment = std::nullopt;
	std::optional<uint16_t> pci_bus = std::nullopt;
	std::optional<uint16_t> pci_slot = std::nullopt;
	std::optional<uint16_t> pci_func = std::nullopt;
	void (*func)(PciDevice *dev);
} quirks[] = {
	{.pci_class = 0x0C, .pci_subclass = 0x03, .pci_interface = 0x00, .func = uhciSmiDisable},
	{.pci_class = 0x0C, .pci_subclass = 0x03, .pci_interface = 0x30, .pci_vendor = 0x8086, .func = switchUsbPortsToXhci},
	{.pci_class = 0x0C, .pci_subclass = 0x03, .pci_interface = 0x30, .pci_vendor = 0x1106, .func = uploadRaspberryPi4Vl805Firmware},
};

} // namespace

void applyPciDeviceQuirks(PciDevice *dev) {
	for (auto [class_id, subclass, interface, vendor, seg, bus, slot, func, handler] : quirks) {
		if(class_id && dev->classCode != *class_id)
			continue;

		if(subclass && dev->subClass != *subclass)
			continue;

		if(interface && dev->interface != *interface)
			continue;

		if(vendor && dev->vendor != *vendor)
			continue;

		if(seg && dev->seg != *seg)
			continue;

		if(bus && dev->bus != *bus)
			continue;

		if(slot && dev->slot != *slot)
			continue;

		if(func && dev->function != *func)
			continue;

		handler(dev);
	}
}

} // namespace thor::pci
