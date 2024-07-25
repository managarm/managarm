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

	std::vector<char> descriptors;
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

struct UsbEndpoint final : sysfs::Object {
	UsbEndpoint(std::string sysfs_name, int64_t mbus_id, std::shared_ptr<drvcore::Device> parent)
		: Object{parent, sysfs_name}, sysfs_name{sysfs_name} {

	}

	protocols::usb::Device &device();

	uint8_t endpointAddress;
	uint8_t attributes;
	uint8_t interval;
	uint8_t length;
	uint16_t maxPacketSize;

	std::string sysfs_name;
};

struct UsbInterface final : UsbBase {
	UsbInterface(std::string sysfs_name, int64_t mbus_id, std::shared_ptr<drvcore::Device> parent)
		: UsbBase{sysfs_name, mbus_id, parent}, sysfs_name{sysfs_name} {

	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		char product[15], interface[9];
		snprintf(product, 15, "%x:%x:%x", desc()->idVendor, desc()->idProduct, desc()->bcdDevice);
		snprintf(interface, 9, "%x/%x/%x", interfaceClass, interfaceSubClass, interfaceProtocol);

		ue.set("DEVTYPE", "usb_interface");
		ue.set("PRODUCT", product);
		ue.set("INTERFACE", interface);
		ue.set("MBUS_ID", std::to_string(mbusId));
	}

	protocols::usb::Device &device();

	uint8_t interfaceClass;
	uint8_t interfaceSubClass;
	uint8_t interfaceProtocol;
	uint8_t alternateSetting;
	uint8_t endpointCount;
	uint8_t interfaceNumber;

	std::vector<std::shared_ptr<UsbEndpoint>> endpoints;

	std::string sysfs_name;
};

struct UsbDevice final : UsbBase {
	UsbDevice(std::string sysfs_name, int64_t mbus_id, std::shared_ptr<drvcore::Device> parent, protocols::usb::Device device)
		: UsbBase{sysfs_name, mbus_id, parent}, _device{device} {

	}

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

	protocols::usb::Device &device() {
		return _device;
	}

	std::vector<std::shared_ptr<UsbInterface>> interfaces;

	size_t maxPower = 0;
	uint8_t bmAttributes = 0;
	uint8_t numInterfaces = 0;

private:
	protocols::usb::Device _device;
};

} // namespace usb_subsystem
