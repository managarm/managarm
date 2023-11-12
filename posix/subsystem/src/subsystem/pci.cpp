
#include <string.h>
#include <iostream>
#include <sstream>

#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>

#include "../common.hpp"
#include "../drvcore.hpp"
#include "../util.hpp"
#include "../vfs.hpp"
#include "fs.bragi.hpp"

namespace pci_subsystem {

drvcore::BusSubsystem *sysfsSubsystem;

std::unordered_map<int, std::shared_ptr<drvcore::Device>> mbusMap;

struct VendorAttribute : sysfs::Attribute {
	VendorAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct DeviceAttribute : sysfs::Attribute {
	DeviceAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct PlainfbAttribute : sysfs::Attribute {
	PlainfbAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct SubsystemVendorAttribute : sysfs::Attribute {
	SubsystemVendorAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct SubsystemDeviceAttribute : sysfs::Attribute {
	SubsystemDeviceAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct ConfigAttribute : sysfs::Attribute {
	ConfigAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false, 256} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct ResourceAttribute : sysfs::Attribute {
	ResourceAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct ResourceNAttribute : sysfs::Attribute {
	ResourceNAttribute(size_t num, std::shared_ptr<drvcore::Device> dev, size_t size)
	: sysfs::Attribute{"resource" + std::to_string(num), true, size}, _device{std::move(dev)}, _barIndex{num} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
	async::result<frg::expected<Error, helix::UniqueDescriptor>> accessMemory(sysfs::Object *object) override;

	std::shared_ptr<drvcore::Device> _device;
	size_t _barIndex;
};

struct ClassAttribute : sysfs::Attribute {
	ClassAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

VendorAttribute vendorAttr{"vendor"};
DeviceAttribute deviceAttr{"device"};
PlainfbAttribute plainfbAttr{"owns_plainfb"};
SubsystemVendorAttribute subsystemVendorAttr{"subsystem_vendor"};
SubsystemDeviceAttribute subsystemDeviceAttr{"subsystem_device"};
ConfigAttribute configAttr{"config"};
ClassAttribute classAttr{"class"};
ResourceAttribute resourceAttr{"resource"};

std::vector<std::shared_ptr<ResourceNAttribute>> resources;

struct Device final : drvcore::BusDevice {
	Device(std::string sysfs_name, int64_t mbus_id, protocols::hw::Device hw_device)
	: drvcore::BusDevice{sysfsSubsystem, std::move(sysfs_name), nullptr},
			mbusId{mbus_id}, _hwDevice{std::move(hw_device)} { }

	void composeUevent(drvcore::UeventProperties &ue) override {
		char slot[13]; // The format is 1234:56:78:9\0.
		snprintf(slot, 13, "0000:%.2x:%.2x.%.1x", pciBus, pciSlot, pciFunction);

		ue.set("SUBSYSTEM", "pci");
		ue.set("PCI_SLOT_NAME", slot);
		ue.set("MBUS_ID", std::to_string(mbusId));
	}

	protocols::hw::Device &hwDevice() {
		return _hwDevice;
	}

	int64_t mbusId;
	uint32_t pciBus;
	uint32_t pciSlot;
	uint32_t pciFunction;
	uint32_t vendorId;
	uint32_t deviceId;
	uint32_t subsystemVendorId;
	uint32_t subsystemDeviceId;
	bool ownsPlainfb = false;

	protocols::hw::Device _hwDevice;
};

async::result<frg::expected<Error, std::string>> VendorAttribute::show(sysfs::Object *object) {
	char buffer[7]; // The format is 0x1234\0.
	auto device = static_cast<Device *>(object);
	snprintf(buffer, 7, "0x%.4x", device->vendorId);
	co_return std::string{buffer};
}

async::result<frg::expected<Error, std::string>> DeviceAttribute::show(sysfs::Object *object) {
	char buffer[7]; // The format is 0x1234\0.
	auto device = static_cast<Device *>(object);
	snprintf(buffer, 7, "0x%.4x", device->deviceId);
	co_return std::string{buffer};
}

async::result<frg::expected<Error, std::string>> SubsystemVendorAttribute::show(sysfs::Object *object) {
	char buffer[7]; // The format is 0x1234\0.
	auto device = static_cast<Device *>(object);
	snprintf(buffer, 7, "0x%.4x", device->subsystemVendorId);
	co_return std::string{buffer};
}

async::result<frg::expected<Error, std::string>> SubsystemDeviceAttribute::show(sysfs::Object *object) {
	char buffer[7]; // The format is 0x1234\0.
	auto device = static_cast<Device *>(object);
	snprintf(buffer, 7, "0x%.4x", device->subsystemDeviceId);
	co_return std::string{buffer};
}

async::result<frg::expected<Error, std::string>> PlainfbAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return device->ownsPlainfb ? "1" : "0";
}

async::result<frg::expected<Error, std::string>> ConfigAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	uint32_t data[256/sizeof(uint32_t)];
	for(size_t i = 0; i < 256/sizeof(uint32_t); i++) {
		data[i] = co_await device->hwDevice().loadPciSpace(i * 4, 4);
	}

	co_return std::string{reinterpret_cast<char *>(data), 256};
}

async::result<frg::expected<Error, std::string>> ClassAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	char buf[10];
	snprintf(buf, 10, "0x%06x\n", co_await device->hwDevice().loadPciSpace(8, 4) >> 8);

	co_return std::string{buf};
}

constexpr size_t IORESOURCE_IO = 0x100;
constexpr size_t IORESOURCE_MEM = 0x200;

async::result<frg::expected<Error, std::string>> ResourceAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);

	std::string res{};
	char buf[58];
	auto info = co_await device->hwDevice().getPciInfo();

	for(auto e : info.barInfo) {
		if(e.hostType == protocols::hw::IoType::kIoTypeNone) {
			res.append("0x0000000000000000 0x0000000000000000 0x0000000000000000\n\0", 58);
		} else if(e.hostType == protocols::hw::IoType::kIoTypeMemory) {
			memset(buf, 0, 58);
			snprintf(buf, 58, "0x%016lx 0x%016lx 0x%016lx\n", e.address, e.address + e.length - 1, IORESOURCE_MEM);
			res.append(buf, 58);
		} else if(e.hostType == protocols::hw::IoType::kIoTypePort) {
			memset(buf, 0, 58);
			snprintf(buf, 58, "0x%016lx 0x%016lx 0x%016lx\n", e.address, e.address + e.length - 1, IORESOURCE_IO);
			res.append(buf, 58);
		}
	}

	co_return res;
}

async::result<frg::expected<Error, helix::UniqueDescriptor>> ResourceNAttribute::accessMemory(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);

	co_return co_await device->hwDevice().accessBar(_barIndex);
}

async::result<frg::expected<Error, std::string>> ResourceNAttribute::show(sysfs::Object *) {
	co_return Error::illegalOperationTarget;
}

async::detached bind(mbus::Entity entity, mbus::Properties properties) {
	std::string sysfs_name = "0000:" + std::get<mbus::StringItem>(properties["pci-bus"]).value
			+ ":" + std::get<mbus::StringItem>(properties["pci-slot"]).value
			+ "." + std::get<mbus::StringItem>(properties["pci-function"]).value;

	// TODO: Add bus/slot/function to this message.
	std::cout << "POSIX: Installing PCI device " << sysfs_name
			<< " (mbus ID: " << entity.getId() << ")" << std::endl;

	protocols::hw::Device hw_device{co_await entity.bind()};

	auto device = std::make_shared<Device>(sysfs_name, entity.getId(), std::move(hw_device));
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
	device->subsystemVendorId = std::stoi(std::get<mbus::StringItem>(
			properties["pci-subsystem-vendor"]).value, 0, 16);
	device->subsystemDeviceId = std::stoi(std::get<mbus::StringItem>(
			properties["pci-subsystem-device"]).value, 0, 16);

	if(properties.find("class") != properties.end()
			&& std::get<mbus::StringItem>(properties["class"]).value == "framebuffer")
		device->ownsPlainfb = true;

	drvcore::installDevice(device);

	// TODO: Call realizeAttribute *before* installing the device.
	device->realizeAttribute(&vendorAttr);
	device->realizeAttribute(&deviceAttr);
	device->realizeAttribute(&plainfbAttr);
	device->realizeAttribute(&subsystemVendorAttr);
	device->realizeAttribute(&subsystemDeviceAttr);
	device->realizeAttribute(&configAttr);
	device->realizeAttribute(&classAttr);
	device->realizeAttribute(&resourceAttr);

	auto info = co_await device->hwDevice().getPciInfo();

	for(size_t i = 0; i < 6; i++) {
		auto e = info.barInfo[i];

		if(e.hostType == protocols::hw::IoType::kIoTypeMemory) {
			auto res = std::make_shared<ResourceNAttribute>(i, device, e.length);
			resources.push_back(res);
			device->realizeAttribute(res.get());
		} else if(e.hostType == protocols::hw::IoType::kIoTypePort) {
			auto res = std::make_shared<ResourceNAttribute>(i, device, e.length);
			resources.push_back(res);
			device->realizeAttribute(res.get());
		}
	}

	mbusMap.insert(std::make_pair(entity.getId(), device));
}

async::detached run() {
	sysfsSubsystem = new drvcore::BusSubsystem{"pci"};

	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("unix.subsystem", "pci")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		bind(std::move(entity), std::move(properties));
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

std::shared_ptr<drvcore::Device> getDeviceByMbus(int id) {
	auto it = mbusMap.find(id);
	assert(it != mbusMap.end());
	return it->second;
}

} // namespace pci_subsystem

