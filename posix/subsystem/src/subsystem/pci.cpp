
#include <string.h>
#include <iostream>
#include <sstream>

#include <async/recurring-event.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>

#include "../common.hpp"
#include "../drvcore.hpp"
#include "../vfs.hpp"
#include "fs.bragi.hpp"
#include "pci.hpp"

namespace pci_subsystem {

drvcore::BusSubsystem *sysfsSubsystem;

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

struct IrqAttribute : sysfs::Attribute {
	IrqAttribute(std::string name)
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
IrqAttribute irqAttr{"irq"};

std::vector<std::shared_ptr<ResourceNAttribute>> resources;

Device::Device(std::string sysfs_name, int64_t mbus_id, protocols::hw::Device hw_device,
	std::shared_ptr<drvcore::Device> parent)
	: drvcore::BusDevice{sysfsSubsystem, std::move(sysfs_name), nullptr, parent},
		mbusId{mbus_id}, _hwDevice{std::move(hw_device)} {
}

struct RootPort final : drvcore::Device {
	RootPort(std::string sysfs_name, int64_t mbus_id, std::shared_ptr<drvcore::Device> parent = nullptr)
	: drvcore::Device{parent, parent, std::move(sysfs_name), nullptr},
			mbusId{mbus_id} { }

	void composeUevent(drvcore::UeventProperties &) override {

	}

	int64_t mbusId;
};

async::result<frg::expected<Error, std::string>> VendorAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return std::format("0x{:04x}\n", device->vendorId);
}

async::result<frg::expected<Error, std::string>> DeviceAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return std::format("0x{:04x}\n", device->deviceId);
}

async::result<frg::expected<Error, std::string>> SubsystemVendorAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return std::format("0x{:04x}\n", device->subsystemVendorId);
}

async::result<frg::expected<Error, std::string>> SubsystemDeviceAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return std::format("0x{:04x}\n", device->subsystemDeviceId);
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

async::result<frg::expected<Error, std::string>> IrqAttribute::show(sysfs::Object *) {
	// TODO: we have no sane way of resolving this yet
	co_return "0\n";
}

async::detached bind(mbus_ng::Entity entity, mbus_ng::Properties properties) {
	auto type = std::get<mbus_ng::StringItem>(properties["pci-type"]).value;

	if(type == "pci-device" || type == "pci-bridge") {
		auto segment = std::get<mbus_ng::StringItem>(properties["pci-segment"]).value;
		auto bus = std::get<mbus_ng::StringItem>(properties["pci-bus"]).value;
		auto parentId = std::stoi(std::get<mbus_ng::StringItem>(properties["drvcore.mbus-parent"]).value);

		std::string sysfs_name = segment + ":" + bus
				+ ":" + std::get<mbus_ng::StringItem>(properties["pci-slot"]).value
				+ "." + std::get<mbus_ng::StringItem>(properties["pci-function"]).value;

		// TODO: Add bus/slot/function to this message.
		std::cout << "POSIX: Installing PCI "
				<< (type == "pci-device" ? "device" : "bridge") << " " << sysfs_name
				<< " (mbus ID: " << entity.id() << ")" << std::endl;

		std::shared_ptr<drvcore::Device> parentObj;

		while(!parentObj) {
			auto ret = drvcore::getMbusDevice(parentId);
			if(ret) {
				parentObj = std::static_pointer_cast<drvcore::Device>(ret);
				break;
			}
			co_await drvcore::mbusMapUpdate.async_wait();
		}

		protocols::hw::Device hwDevice{(co_await entity.getRemoteLane()).unwrap()};

		auto device = std::make_shared<Device>(sysfs_name, entity.id(), std::move(hwDevice), parentObj);
		device->pciSegment = std::stoi(segment, nullptr, 16);
		device->pciBus = std::stoi(std::get<mbus_ng::StringItem>(
				properties["pci-bus"]).value, nullptr, 16);
		device->pciSlot = std::stoi(std::get<mbus_ng::StringItem>(
				properties["pci-slot"]).value, nullptr, 16);
		device->pciFunction = std::stoi(std::get<mbus_ng::StringItem>(
				properties["pci-function"]).value, nullptr, 16);
		device->vendorId = std::stoi(std::get<mbus_ng::StringItem>(
				properties["pci-vendor"]).value, nullptr, 16);
		device->deviceId = std::stoi(std::get<mbus_ng::StringItem>(
				properties["pci-device"]).value, nullptr, 16);
		device->pciClass = std::stoi(std::get<mbus_ng::StringItem>(
				properties["pci-class"]).value, nullptr, 16);
		device->pciSubclass = std::stoi(std::get<mbus_ng::StringItem>(
				properties["pci-subclass"]).value, nullptr, 16);
		device->pciProgif = std::stoi(std::get<mbus_ng::StringItem>(
				properties["pci-interface"]).value, nullptr, 16);
		if(type == "pci-device") {
			device->subsystemVendorId = std::stoi(std::get<mbus_ng::StringItem>(
					properties["pci-subsystem-vendor"]).value, nullptr, 16);
			device->subsystemDeviceId = std::stoi(std::get<mbus_ng::StringItem>(
					properties["pci-subsystem-device"]).value, nullptr, 16);
		}

		if(properties.find("class") != properties.end()
				&& std::get<mbus_ng::StringItem>(properties["class"]).value == "framebuffer")
			device->ownsPlainfb = true;

		drvcore::installDevice(device);

		// TODO: Call realizeAttribute *before* installing the device.
		device->realizeAttribute(&vendorAttr);
		device->realizeAttribute(&deviceAttr);
		device->realizeAttribute(&plainfbAttr);
		if(type == "pci-device") {
			device->realizeAttribute(&subsystemVendorAttr);
			device->realizeAttribute(&subsystemDeviceAttr);
		}
		device->realizeAttribute(&configAttr);
		device->realizeAttribute(&classAttr);
		device->realizeAttribute(&resourceAttr);
		device->realizeAttribute(&irqAttr);

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

		drvcore::registerMbusDevice(entity.id(), device);
	} else if(type == "pci-root-bus") {
		auto segment = std::get<mbus_ng::StringItem>(properties["pci-segment"]).value;
		auto bus = std::get<mbus_ng::StringItem>(properties["pci-bus"]).value;

		std::string sysfs_name = "pci" + segment + ":" + bus;

		auto device = std::make_shared<RootPort>(sysfs_name, entity.id());
		drvcore::installDevice(device);

		std::cout << "POSIX: Installed PCI root bus " << sysfs_name
				<< " (mbus ID: " << entity.id() << ")" << std::endl;

		drvcore::registerMbusDevice(entity.id(), device);
	} else {
		std::cout << "posix: unsupported PCI dev type '" << type << "'" << std::endl;
		assert(!"unsupported");
	}
}

async::detached run() {
	sysfsSubsystem = new drvcore::BusSubsystem{"pci"};
	sysfsSubsystem->object()->directoryNode()->directMkdir("slots");

	auto filter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter{"unix.subsystem", "pci"}
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);

			bind(std::move(entity), std::move(event.properties));
		}
	}
}

} // namespace pci_subsystem

