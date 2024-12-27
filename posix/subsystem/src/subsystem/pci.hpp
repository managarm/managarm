#pragma once

#include <async/result.hpp>
#include <protocols/hw/client.hpp>
#include "../drvcore.hpp"

namespace pci_subsystem {

async::detached run();

struct Device final : drvcore::BusDevice {
	Device(std::string sysfs_name, int64_t mbus_id, protocols::hw::Device hw_device, std::shared_ptr<drvcore::Device> parent = nullptr);

	void composeUevent(drvcore::UeventProperties &ue) override {
		char slot[13]; // The format is 1234:56:78:9\0.
		snprintf(slot, 13, "0000:%.2x:%.2x.%.1x", pciBus, pciSlot, pciFunction);

		ue.set("SUBSYSTEM", "pci");
		ue.set("PCI_SLOT_NAME", slot);
		ue.set("PCI_CLASS", std::format("{:X}{:02X}{:02X}", pciClass, pciSubclass, pciProgif));
		ue.set("MBUS_ID", std::to_string(mbusId));
	}

	protocols::hw::Device &hwDevice() {
		return _hwDevice;
	}

	int64_t mbusId;
	uint32_t pciSegment;
	uint32_t pciBus;
	uint32_t pciSlot;
	uint32_t pciFunction;
	uint8_t pciClass;
	uint8_t pciSubclass;
	uint8_t pciProgif;
	uint32_t vendorId;
	uint32_t deviceId;
	uint32_t subsystemVendorId;
	uint32_t subsystemDeviceId;
	bool ownsPlainfb = false;

	protocols::hw::Device _hwDevice;
};

} // namespace pci_subsystem
