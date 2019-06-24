
#include <string.h>
#include <iostream>
#include <sstream>

#include <cofiber.hpp>
#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../drvcore.hpp"
#include "../util.hpp"
#include "../vfs.hpp"
#include "fs.pb.h"

namespace pci_subsystem {

drvcore::BusSubsystem *sysfsSubsystem;

std::unordered_map<int, std::shared_ptr<drvcore::Device>> mbusMap;

struct VendorAttribute : sysfs::Attribute {
	VendorAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct DeviceAttribute : sysfs::Attribute {
	DeviceAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct PlainfbAttribute : sysfs::Attribute {
	PlainfbAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

VendorAttribute vendorAttr{"vendor"};
DeviceAttribute deviceAttr{"device"};
PlainfbAttribute plainfbAttr{"owns_plainfb"};

struct Device final : drvcore::BusDevice {
	Device(std::string sysfs_name)
	: drvcore::BusDevice{sysfsSubsystem, std::move(sysfs_name), nullptr} { }

	void composeUevent(drvcore::UeventProperties &ue) override {
		char slot[13]; // The format is 1234:56:78:9\0.
		sprintf(slot, "0000:%.2x:%.2x.%.1x", pciBus, pciSlot, pciFunction);

		ue.set("SUBSYSTEM", "pci");
		ue.set("PCI_SLOT_NAME", slot);
	}

	uint32_t pciBus;
	uint32_t pciSlot;
	uint32_t pciFunction;
	uint32_t vendorId;
	uint32_t deviceId;
	bool ownsPlainfb = false;
};

async::result<std::string> VendorAttribute::show(sysfs::Object *object) {
	char buffer[7]; // The format is 0x1234\0.
	auto device = static_cast<Device *>(object);
	sprintf(buffer, "0x%.4x", device->vendorId);
	co_return std::string{buffer};
}

async::result<std::string> DeviceAttribute::show(sysfs::Object *object) {
	char buffer[7]; // The format is 0x1234\0.
	auto device = static_cast<Device *>(object);
	sprintf(buffer, "0x%.4x", device->deviceId);
	co_return std::string{buffer};
}

async::result<std::string> PlainfbAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return device->ownsPlainfb ? "1" : "0";
}

async::detached run() {
	sysfsSubsystem = new drvcore::BusSubsystem{"pci"};

	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("unix.subsystem", "pci")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		std::string sysfs_name = "0000:" + std::get<mbus::StringItem>(properties["pci-bus"]).value
				+ ":" + std::get<mbus::StringItem>(properties["pci-slot"]).value
				+ "." + std::get<mbus::StringItem>(properties["pci-function"]).value;

		// TODO: Add bus/slot/function to this message.
		std::cout << "POSIX: Installing PCI device " << sysfs_name
				<< " (mbus ID: " << entity.getId() << ")" << std::endl;

		auto device = std::make_shared<Device>(sysfs_name);
		device->pciBus = std::stoi(std::get<mbus::StringItem>(
				properties["pci-bus"]).value, 0, 16);
		device->pciSlot = std::stoi(std::get<mbus::StringItem>(
				properties["pci-slot"]).value, 0, 16);
		device->pciFunction = std::stoi(std::get<mbus::StringItem>(
				properties["pci-function"]).value, 0, 16);
		device->vendorId = std::stoi(std::get<mbus::StringItem>(
				properties["pci-vendor"]).value, 0, 16);
		device->deviceId = std::stoi(std::get<mbus::StringItem>(
				properties["pci-device"]).value, 0, 16);
		if(properties.find("class") != properties.end()
				&& std::get<mbus::StringItem>(properties["class"]).value == "framebuffer")
			device->ownsPlainfb = true;

		drvcore::installDevice(device);
		// TODO: Call realizeAttribute *before* installing the device.
		device->realizeAttribute(&vendorAttr);
		device->realizeAttribute(&deviceAttr);
		device->realizeAttribute(&plainfbAttr);

		mbusMap.insert(std::make_pair(entity.getId(), device));
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

std::shared_ptr<drvcore::Device> getDeviceByMbus(int id) {
	auto it = mbusMap.find(id);
	assert(it != mbusMap.end());
	return it->second;
}

} // namespace pci_subsystem

