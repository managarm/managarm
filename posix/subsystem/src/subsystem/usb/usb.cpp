#include <string.h>
#include <iostream>

#include <protocols/mbus/client.hpp>

#include "../../util.hpp"
#include "../pci.hpp"
#include "usb.hpp"

namespace {

std::unordered_map<mbus::_detail::EntityId, uint64_t> usbControllerMap;
id_allocator<uint64_t> usbControllerAllocator;

}

namespace usb_subsystem {

drvcore::BusSubsystem *sysfsSubsystem;

std::unordered_map<int, std::shared_ptr<drvcore::Device>> mbusMap;


async::detached bindController(mbus::Entity entity, mbus::Properties properties, uint64_t bus_num) {
	auto pci_parent_id = std::stoi(std::get<mbus::StringItem>(properties["usb.root.parent"]).value);
	auto pci = pci_subsystem::getDeviceByMbus(pci_parent_id);

	auto sysfs_name = "usb" + std::to_string(bus_num);

	auto version_major_str = std::get<mbus::StringItem>(properties["usb.version.major"]);
	auto version_minor_str = std::get<mbus::StringItem>(properties["usb.version.minor"]);
	co_return;
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
}

std::shared_ptr<drvcore::Device> getDeviceByMbus(int id) {
	auto it = mbusMap.find(id);

	if(it != mbusMap.end())
		return it->second;

	return nullptr;
}

} // namespace usb_subsystem
