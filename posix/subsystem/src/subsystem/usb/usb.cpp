#include <string.h>
#include <iostream>

#include <async/queue.hpp>
#include <protocols/mbus/client.hpp>

#include <protocols/usb/api.hpp>
#include <protocols/usb/client.hpp>

#include "../../drvcore.hpp"
#include "../../util.hpp"
#include "../pci.hpp"
#include "attributes.hpp"
#include "devices.hpp"
#include "root-hub.hpp"
#include "usb.hpp"

namespace {

std::unordered_map<mbus_ng::EntityId, uint64_t> usbControllerMap;
id_allocator<uint64_t> usbControllerAllocator;

} // namespace anonymous

namespace usb_subsystem {

drvcore::BusSubsystem *sysfsSubsystem;

std::unordered_map<int, std::shared_ptr<drvcore::Device>> mbusMap;

protocols::usb::Device &UsbInterface::device() {
	return std::static_pointer_cast<UsbDevice>(parentDevice())->device();
}

VendorAttribute vendorAttr{"idVendor"};
DeviceAttribute deviceAttr{"idProduct"};
DeviceClassAttribute deviceClassAttr{"bDeviceClass"};
DeviceSubClassAttribute deviceSubClassAttr{"bDeviceSubClass"};
DeviceProtocolAttribute deviceProtocolAttr{"bDeviceProtocol"};
BcdDeviceAttribute bcdDeviceAttr{"bcdDevice"};
VersionAttribute versionAttr{"version"};
SpeedAttribute speedAttr{"speed"};
DeviceMaxPowerAttribute deviceMaxPowerAttr{"bMaxPower"};
ControllerMaxPowerAttribute controllerMaxPowerAttr{"bMaxPower"};
MaxChildAttribute maxChildAttr{"maxchild"};
NumInterfacesAttribute numInterfacesAttr{"bNumInterfaces"};
BusNumAttribute busNumAttr{"busnum"};
DevNumAttribute devNumAttr{"devnum"};
DescriptorsAttribute descriptorsAttr{"descriptors"};
RxLanesAttribute rxLanesAttr{"rx_lanes"};
TxLanesAttribute txLanesAttr{"tx_lanes"};
ConfigValueAttribute configValueAttr{"bConfigurationValue"};
MaxPacketSize0Attribute maxPacketSize0Attr{"bMaxPacketSize0"};
ConfigurationAttribute configurationAttr{"configuration"};
BmAttributesAttribute bmAttributesAttr{"bmAttributes"};
NumConfigurationsAttribute numConfigurationsAttr{"bNumConfigurations"};

/* USB Interface-specific attributes */
InterfaceClassAttribute interfaceClassAttr{"bInterfaceClass"};
InterfaceSubClassAttribute interfaceSubClassAttr{"bInterfaceSubClass"};
InterfaceProtocolAttribute interfaceProtocolAttr{"bInterfaceProtocol"};
AlternateSettingAttribute alternateSettingAttr{"bAlternateSetting"};
InterfaceNumberAttribute interfaceNumAttr{"bInterfaceNumber"};
EndpointNumAttribute numEndpointsAttr{"bNumEndpoints"};

void bindController(mbus_ng::Entity entity, mbus_ng::Properties properties, uint64_t bus_num) {
	auto pci_parent_id = std::stoi(std::get<mbus_ng::StringItem>(properties["usb.root.parent"]).value);
	auto pci = pci_subsystem::getDeviceByMbus(pci_parent_id);
	assert(pci);

	auto sysfs_name = "usb" + std::to_string(bus_num);
	auto device = std::make_shared<UsbController>(sysfs_name, entity.id(), pci);
	/* set up the /sys/bus/usb/devices/usbX symlink  */
	sysfsSubsystem->devicesObject()->createSymlink(sysfs_name, device);

	auto version_major_str = std::get<mbus_ng::StringItem>(properties["usb.version.major"]);
	auto version_minor_str = std::get<mbus_ng::StringItem>(properties["usb.version.minor"]);

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
				case 0x10:
					device->speed = "10000";
					device->descriptors.insert(device->descriptors.end(), root_hub::descUsb3_1.begin(), root_hub::descUsb3_1.end());
					device->descriptors.insert(device->descriptors.end(), root_hub::descSuperSpeed.begin(), root_hub::descSuperSpeed.end());
					break;
				case 0x20:
					device->speed = "20000";
					device->descriptors.insert(device->descriptors.end(), root_hub::descUsb3_1.begin(), root_hub::descUsb3_1.end());
					device->descriptors.insert(device->descriptors.end(), root_hub::descSuperSpeed.begin(), root_hub::descSuperSpeed.end());
					break;
				default:
					std::cerr << "unhandled USB 3 minor revision: " << minor << '\n';
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

	device->realizeAttribute(&vendorAttr);
	device->realizeAttribute(&deviceAttr);
	device->realizeAttribute(&deviceClassAttr);
	device->realizeAttribute(&deviceSubClassAttr);
	device->realizeAttribute(&deviceProtocolAttr);
	device->realizeAttribute(&versionAttr);
	device->realizeAttribute(&speedAttr);
	device->realizeAttribute(&controllerMaxPowerAttr);
	device->realizeAttribute(&maxChildAttr);
	device->realizeAttribute(&numInterfacesAttr);
	device->realizeAttribute(&busNumAttr);
	device->realizeAttribute(&devNumAttr);
	device->realizeAttribute(&descriptorsAttr);
	device->realizeAttribute(&rxLanesAttr);
	device->realizeAttribute(&txLanesAttr);

	mbusMap.insert({entity.id(), device});
}

async::result<void> bindDevice(mbus_ng::Entity entity, mbus_ng::Properties properties) {
	auto address = std::get<mbus_ng::StringItem>(properties["usb.hub_port"]);
	auto mbus_bus = std::get<mbus_ng::StringItem>(properties["usb.bus"]);
	uint64_t bus = std::stol(mbus_bus.value);
	auto parent = getDeviceByMbus(bus);

	assert(usbControllerMap.find(bus) != usbControllerMap.end());
	auto bus_num = usbControllerMap[bus];

	auto sysfs_name = std::to_string(bus_num) + "-" + std::to_string(std::stoi(address.value, 0, 16));

	std::cout << "POSIX: Installing USB device " << sysfs_name << " (mbus ID: " << entity.id() << ")" << std::endl;

	auto lane = (co_await entity.getRemoteLane()).unwrap();
	auto hw = protocols::usb::connect(std::move(lane));

	auto device = std::make_shared<UsbDevice>(sysfs_name, entity.id(), parent, std::move(hw));

	/* obtain the device descroptor */
	auto raw_dev_desc = (co_await device->device().deviceDescriptor()).value();
	/* obtain the tree of configuration descriptors and its subdescriptors */
	auto raw_descs = (co_await device->device().configurationDescriptor()).value();

	device->portNum = std::stoi(address.value) + 1;
	device->busNum = bus_num;
	device->speed = std::get<mbus_ng::StringItem>(properties["usb.speed"]).value;

	device->descriptors.insert(device->descriptors.end(), raw_dev_desc.begin(), raw_dev_desc.end());
	device->descriptors.insert(device->descriptors.end(), raw_descs.begin(), raw_descs.end());

	auto config_val = (co_await device->device().currentConfigurationValue()).value();

	protocols::usb::walkConfiguration(raw_descs, [&] (int type, size_t, void *descriptor, const auto &info) {
		if(type == protocols::usb::descriptor_type::configuration) {
			auto desc = reinterpret_cast<protocols::usb::ConfigDescriptor *>(descriptor);
			device->maxPower = desc->maxPower * 2;

			if(info.configNumber == config_val) {
				device->bmAttributes = desc->bmAttributes;
				device->numInterfaces = reinterpret_cast<protocols::usb::ConfigDescriptor *>(descriptor)->numInterfaces;
			}
		} else if(type == protocols::usb::descriptor_type::interface) {
			auto desc = reinterpret_cast<protocols::usb::InterfaceDescriptor *>(descriptor);

			auto if_sysfs_name = sysfs_name + ":" + std::to_string(*info.configNumber) + "-" + std::to_string(desc->interfaceNumber);
			auto interface = std::make_shared<UsbInterface>(if_sysfs_name, entity.id(), device);

			interface->interfaceClass = desc->interfaceClass;
			interface->interfaceSubClass = desc->interfaceSubClass;
			interface->interfaceProtocol = desc->interfaceProtocol;
			interface->alternateSetting = desc->alternateSetting;
			interface->interfaceNumber = desc->interfaceNumber;
			interface->endpoints = desc->numEndpoints;
			interface->descriptors = device->descriptors;

			device->interfaces.push_back(interface);
		}
	});

	drvcore::installDevice(device);
	sysfsSubsystem->devicesObject()->createSymlink(sysfs_name, device);

	for(auto interface : device->interfaces) {
		if(interface->alternateSetting != 0) {
			// TODO(no92): currently we don't support anything but bAlternateSetting 0
			continue;
		}

		drvcore::installDevice(interface);
		sysfsSubsystem->devicesObject()->createSymlink(interface->sysfs_name, interface);

		interface->realizeAttribute(&interfaceClassAttr);
		interface->realizeAttribute(&interfaceSubClassAttr);
		interface->realizeAttribute(&interfaceProtocolAttr);
		interface->realizeAttribute(&alternateSettingAttr);
		interface->realizeAttribute(&interfaceNumAttr);
		interface->realizeAttribute(&numEndpointsAttr);
	}

	// TODO: Call realizeAttribute *before* installing the device.
	device->realizeAttribute(&vendorAttr);
	device->realizeAttribute(&deviceAttr);
	device->realizeAttribute(&deviceClassAttr);
	device->realizeAttribute(&deviceSubClassAttr);
	device->realizeAttribute(&deviceProtocolAttr);
	device->realizeAttribute(&bcdDeviceAttr);

	device->realizeAttribute(&versionAttr);
	device->realizeAttribute(&speedAttr);
	device->realizeAttribute(&deviceMaxPowerAttr);
	device->realizeAttribute(&maxChildAttr);
	device->realizeAttribute(&numInterfacesAttr);
	device->realizeAttribute(&busNumAttr);
	device->realizeAttribute(&devNumAttr);
	device->realizeAttribute(&descriptorsAttr);
	device->realizeAttribute(&rxLanesAttr);
	device->realizeAttribute(&txLanesAttr);
	device->realizeAttribute(&configValueAttr);
	device->realizeAttribute(&maxPacketSize0Attr);
	device->realizeAttribute(&configurationAttr);
	device->realizeAttribute(&bmAttributesAttr);
	device->realizeAttribute(&numConfigurationsAttr);

	mbusMap.insert({entity.id(), device});
}

async::detached observeDevicesOnController(mbus_ng::EntityId controllerId) {
	auto usbDeviceFilter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter{"unix.subsystem", "usb"},
		mbus_ng::EqualsFilter{"usb.bus", std::to_string(controllerId)},
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(usbDeviceFilter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);

			co_await bindDevice(std::move(entity), std::move(event.properties));
		}
	}
}

async::detached run() {
	usbControllerAllocator.use_range();

	sysfsSubsystem = new drvcore::BusSubsystem{"usb"};

	auto usbControllerFilter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter{"generic.devtype", "usb-controller"}
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(usbControllerFilter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);

			auto id = usbControllerAllocator.allocate();
			usbControllerMap.insert({entity.id(), id});
			bindController(std::move(entity), std::move(event.properties), id);
			observeDevicesOnController(entity.id());
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
