#include <cstdint>
#include <frg/optional.hpp>

#include <thor-internal/debug.hpp>
#include <thor-internal/pci/pci.hpp>

namespace thor::pci {

namespace {

void uhciSmiDisable(smarter::shared_ptr<pci::PciDevice> dev) {
	debugLogger() << "            Disabling UHCI SMI generation!" << frg::endlog;
	dev->parentBus->io->writeConfigHalf(dev->parentBus, dev->slot, dev->function, 0xC0, 0x2000);
}

void switchUsbPortsToXhci(smarter::shared_ptr<pci::PciDevice> dev) {
	debugLogger() << "            Switching USB ports to XHCI!" << frg::endlog;
	auto io = dev->parentBus->io;

	auto usb3PortsAvail = io->readConfigWord(dev->parentBus, dev->slot, dev->function, 0xDC);
	io->writeConfigWord(dev->parentBus, dev->slot, dev->function, 0xD8, usb3PortsAvail);

	auto usb2PortsAvail = io->readConfigWord(dev->parentBus, dev->slot, dev->function, 0xD4);
	io->writeConfigWord(dev->parentBus, dev->slot, dev->function, 0xD0, usb2PortsAvail);
}

struct {
	int pci_class;
	int pci_subclass;
	int pci_interface;
	int pci_vendor;
	long int pci_bus;
	long int pci_slot;
	long int pci_func;
	void (*func)(smarter::shared_ptr<pci::PciDevice> dev);
} quirks[] = {
	{0x0C, 0x03, 0x00, -1, -1, -1, -1, uhciSmiDisable},
	{0x0C, 0x03, 0x30, 0x8086, -1, -1, -1, switchUsbPortsToXhci},
};

} // namespace

void applyPciDeviceQuirks(smarter::shared_ptr<pci::PciDevice> dev) {
	for(auto [class_id, subclass, interface, vendor, bus, slot, func, handler] : quirks) {
		if(class_id >= 0 && dev->classCode != class_id)
			continue;

		if(subclass >= 0 && dev->subClass != subclass)
			continue;

		if(interface >= 0 && dev->interface != interface)
			continue;

		if(vendor >= 0 && dev->vendor != vendor)
			continue;

		if(bus >= 0 && dev->bus != bus)
			continue;

		if(slot >= 0 && dev->slot != slot)
			continue;

		if(func >= 0 && dev->function != func)
			continue;

		handler(dev);
	}
}

} // namespace thor::pci
