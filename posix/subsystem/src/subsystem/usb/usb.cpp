#include <string.h>
#include <iostream>

#include <async/queue.hpp>
#include <protocols/mbus/client.hpp>

#include <protocols/usb/api.hpp>
#include <protocols/usb/client.hpp>

#include "../../drvcore.hpp"
#include "../../util.hpp"
#include "../pci.hpp"
#include "devices.hpp"
#include "root-hub.hpp"
#include "usb.hpp"

namespace {

std::unordered_map<mbus::_detail::EntityId, uint64_t> usbControllerMap;
id_allocator<uint64_t> usbControllerAllocator;
async::queue<uint64_t, frg::stl_allocator> controllerDetectedQueue;

}

namespace usb_subsystem {

drvcore::BusSubsystem *sysfsSubsystem;

std::unordered_map<int, std::shared_ptr<drvcore::Device>> mbusMap;


async::detached bindController(mbus::Entity entity, mbus::Properties properties, uint64_t bus_num) {
	auto pci_parent_id = std::stoi(std::get<mbus::StringItem>(properties["usb.root.parent"]).value);
	auto pci = pci_subsystem::getDeviceByMbus(pci_parent_id);

	auto sysfs_name = "usb" + std::to_string(bus_num);
	auto device = std::make_shared<UsbController>(sysfs_name, entity.getId(), pci);
	/* set up the /sys/bus/usb/devices/usbX symlink  */
	sysfsSubsystem->devicesObject()->createSymlink(sysfs_name, device);

	auto version_major_str = std::get<mbus::StringItem>(properties["usb.version.major"]);
	auto version_minor_str = std::get<mbus::StringItem>(properties["usb.version.minor"]);

	device->busNum = bus_num;
	device->portNum = 1;
	device->numInterfaces = 1;

	auto major = std::stoi(version_major_str.value);
	auto minor = std::stoi(version_minor_str.value);

	switch(major) {
		case 1:
			switch(minor) {
				case 0:
					device->speed = "1.5";
					break;
				case 0x10:
					device->speed = "12";
					break;
				default:
					printf("USB version 1.%u\n", minor);
					assert(!"invalid USB 1 minor revision");
					break;
			}
			device->descriptors.insert(device->descriptors.end(), root_hub::descUsb1_1.begin(), root_hub::descUsb1_1.end());
			device->descriptors.insert(device->descriptors.end(), root_hub::descFullSpeed.begin(), root_hub::descFullSpeed.end());
			break;
		case 2:
			device->speed = "480"; // High speed
			device->descriptors.insert(device->descriptors.end(), root_hub::descUsb2_0.begin(), root_hub::descUsb2_0.end());
			device->descriptors.insert(device->descriptors.end(), root_hub::descHighSpeed.begin(), root_hub::descHighSpeed.end());
			break;
		case 3:
			switch(minor) {
				case 0:
					device->speed = "5000";
				device->descriptors.insert(device->descriptors.end(), root_hub::descUsb3_0.begin(), root_hub::descUsb3_0.end());
				device->descriptors.insert(device->descriptors.end(), root_hub::descSuperSpeed.begin(), root_hub::descSuperSpeed.end());
					break;
				default:
					assert(!"unhandled USB 3 minor revision");
					break;
			}
			break;
		default:
			device->speed = "unknown";
			break;
	}

	assert(device->descriptors.size() >= 18 + 25);

	drvcore::installDevice(device);

	mbusMap.insert({entity.getId(), device});

	controllerDetectedQueue.put(entity.getId());

	co_return;
}

async::detached bindDevice(mbus::Entity entity, mbus::Properties properties) {
	auto address = std::get<mbus::StringItem>(properties["usb.hub_port"]);
	auto mbus_bus = std::get<mbus::StringItem>(properties["usb.bus"]);
	uint64_t bus = std::stol(mbus_bus.value);
	auto parent = getDeviceByMbus(bus);

	assert(usbControllerMap.find(bus) != usbControllerMap.end());
	auto bus_num = usbControllerMap[bus];

	auto sysfs_name = std::to_string(bus_num) + "-" + std::to_string(std::stoi(address.value, 0, 16));

	std::cout << "POSIX: Installing USB device " << sysfs_name << " (mbus ID: " << entity.getId() << ")" << std::endl;
}

async::detached run() {
	usbControllerAllocator.use_range();

	sysfsSubsystem = new drvcore::BusSubsystem{"usb"};

	auto root = co_await mbus::Instance::global().getRoot();

	auto usbControllerFilter = mbus::Conjunction({
		mbus::EqualsFilter("generic.devtype", "usb-controller")
	});

	auto usbControllerHandler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		auto id = usbControllerAllocator.allocate();
		usbControllerMap.insert({entity.getId(), id});
		bindController(std::move(entity), std::move(properties), id);
	});

	co_await root.linkObserver(std::move(usbControllerFilter), std::move(usbControllerHandler));

	while(1) {
		auto id = co_await controllerDetectedQueue.async_get();

		if(id) {
			auto filter = mbus::Conjunction({
				mbus::EqualsFilter("unix.subsystem", "usb"),
				mbus::EqualsFilter("usb.bus", std::to_string(*id)),
			});

			auto handler = mbus::ObserverHandler{}
			.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
				bindDevice(std::move(entity), std::move(properties));
			});

			co_await root.linkObserver(std::move(filter), std::move(handler));
		}
	}
}

std::shared_ptr<drvcore::Device> getDeviceByMbus(int id) {
	auto it = mbusMap.find(id);

	if(it != mbusMap.end())
		return it->second;

	return nullptr;
}

} // namespace usb_subsystem
