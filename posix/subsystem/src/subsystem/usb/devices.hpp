#pragma once

#include "../../drvcore.hpp"
#include "protocols/usb/client.hpp"

namespace usb_subsystem {

struct UsbBase : drvcore::Device {
protected:
	UsbBase(std::string sysfs_name, int64_t mbus_id, std::shared_ptr<drvcore::Device> parent)
	: drvcore::Device{parent, std::move(sysfs_name), nullptr},
			mbusId{mbus_id} { }

public:
	protocols::usb::DeviceDescriptor *desc() {
		return reinterpret_cast<protocols::usb::DeviceDescriptor *>(descriptors.data());
	}

	int64_t mbusId;
	size_t busNum;
	size_t portNum;
	std::string speed;

	std::vector<uint8_t> descriptors;
};

struct UsbController final : UsbBase {
	UsbController(std::string sysfs_name, int64_t mbus_id, std::shared_ptr<drvcore::Device> parent)
		: UsbBase{sysfs_name, mbus_id, parent} { }

	void composeUevent(drvcore::UeventProperties &ue) override {
		char product[15];
		snprintf(product, 15, "%x:%x:%x", desc()->idVendor, desc()->idProduct, desc()->bcdDevice);
		char devname[16];
		char busnum[4];
		char devnum[4];
		snprintf(devname, 16, "bus/usb/%03zu/%03zu", busNum, portNum);
		snprintf(busnum, 4, "%03zu", busNum);
		snprintf(devnum, 4, "%03zu", portNum);

		ue.set("DEVTYPE", "usb_device");
		ue.set("DEVNAME", devname);
		ue.set("PRODUCT", product);
		ue.set("SUBSYSTEM", "usb");
		ue.set("BUSNUM", busnum);
		ue.set("DEVNUM", devnum);
		ue.set("MBUS_ID", std::to_string(mbusId));
	}

	uint8_t maxPower = 0;
	uint8_t bmAttributes = 0;
	uint8_t numInterfaces = 0;
};

} // namespace usb_subsystem
