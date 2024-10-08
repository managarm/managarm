#include <cstdint>
#include <frg/optional.hpp>

#include <thor-internal/debug.hpp>
#include <thor-internal/pci/intel-igd.hpp>
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

void readIntelIntegratedGraphicsVbt(smarter::shared_ptr<pci::PciDevice> dev) {
	auto io = dev->parentBus->io;

	uint32_t asls_phys = io->readConfigWord(dev->parentBus, dev->slot, dev->function, 0xFC);
	if(!asls_phys) {
		// ACPI OpRegion not supported
		return;
	}

	infoLogger() << "            OpRegion physical address " << frg::hex_fmt{asls_phys} << frg::endlog;

	uint32_t asls_phys_aligned = asls_phys & ~0xFFF;
	uint32_t asls_offset = asls_phys & 0xFFF;
	size_t asls_pages = (asls_offset) ? 3 : 2;
	auto opregion = reinterpret_cast<IgdOpregionHeader *>(uintptr_t(KernelVirtualMemory::global().allocate(asls_pages * 0x1000)) + asls_offset);

	for(size_t i = 0; i < asls_pages; i++) {
		KernelPageSpace::global().mapSingle4k((VirtualAddr(opregion) & ~0xFFF) + (i * 0x1000), asls_phys_aligned + (i * 0x1000), page_access::write, CachingMode::uncached);
	}

	if(memcmp(opregion, IGD_OPREGION_SIGNATURE, 16)) {
		return;
	}

	infoLogger() << "            \e[32mfound ACPI OpRegion " << opregion->over.major << "." <<
		opregion->over.minor << "." << opregion->over.revision << "\e[39m" << frg::endlog;

	IgdOpregionAsle *asle = NULL;

	if(opregion->mbox & (1 << 2)) {
		asle = reinterpret_cast<IgdOpregionAsle *>((uintptr_t) opregion + IGD_OPREGION_ASLE_OFFSET);
	}

	if(opregion->over.major >= 2 && asle && asle->rvda && asle->rvds) {
		uint64_t rvda = asle->rvda;

		// In OpRegion v2.1+, rvda was changed to a relative offset
		if(opregion->over.major > 2 || (opregion->over.major == 2 && opregion->over.minor >= 1)) {
			if(rvda < IGD_OPREGION_SIZE) {
				infoLogger() << "            \e[33mVBT base shouldn't be within OpRegion, but it is!" << "\e[39m" << frg::endlog;
			}

			rvda += asls_phys;
		}

		/* OpRegion 2.0: rvda is a physical address */
		auto vbt = smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
			rvda & ~(kPageSize - 1), (asle->rvds + (kPageSize - 1)) & ~(kPageSize - 1),
			CachingMode::uncached);

		dev->igdVbt = std::move(vbt);
		return;
	}

	if(!(opregion->mbox & (1 << 3))) {
		// ACPI OpRegion does not support VBT mailbox when it should
		return;
	}

	size_t vbt_size = ((opregion->mbox & (1 << 4)) ? IGD_OPREGION_ASLE_EXT_OFFSET : IGD_OPREGION_SIZE) - IGD_OPREGION_VBT_OFFSET;

	auto vbt = smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
		(asls_phys + IGD_OPREGION_VBT_OFFSET) & ~(kPageSize - 1),
		(vbt_size + (kPageSize - 1)) & ~(kPageSize - 1),
		CachingMode::uncached);

	dev->igdVbt = std::move(vbt);
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
	{0x03, 0x00, -1, 0x8086, 0, 2, 0, readIntelIntegratedGraphicsVbt},
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
