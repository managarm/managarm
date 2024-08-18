#include <string.h>
#include <iostream>
#include <format>

#include <async/queue.hpp>
#include <protocols/mbus/client.hpp>

#include <core/id-allocator.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/client.hpp>

#include "../../drvcore.hpp"
#include "../generic.hpp"
#include "../net.hpp"
#include "../pci.hpp"
#include "../usbmisc.hpp"
#include "attributes.hpp"
#include "devices.hpp"
#include "drivers.hpp"
#include "root-hub.hpp"
#include "usb.hpp"

using protocols::usb::DescriptorBase;

namespace {

std::unordered_map<mbus_ng::EntityId, uint64_t> usbControllerMap;
id_allocator<uint64_t> usbControllerAllocator{};

} // namespace anonymous

namespace usb_subsystem {

std::shared_ptr<drvcore::BusSubsystem> sysfsSubsystem;
drvcore::ClassSubsystem *netSubsystem;
drvcore::ClassSubsystem *usbmiscSubsystem;

protocols::usb::Device &UsbInterface::device() {
	return std::static_pointer_cast<UsbDevice>(parentDevice())->device();
}

VendorAttribute vendorAttr{"idVendor"};
DeviceAttribute deviceAttr{"idProduct"};
DeviceClassAttribute deviceClassAttr{"bDeviceClass"};
DeviceSubClassAttribute deviceSubClassAttr{"bDeviceSubClass"};
DeviceProtocolAttribute deviceProtocolAttr{"bDeviceProtocol"};
BcdDeviceAttribute bcdDeviceAttr{"bcdDevice"};
ManufacturerNameAttribute manufacturerNameAttr{"manufacturer"};
ProductNameAttribute productNameAttr{"product"};
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

/* USB Endpoint-specific attributes */
EndpointAddressAttribute endpointAddressAttr{"bEndpointAddress"};
PrettyIntervalAttribute prettyIntervalAttr{"interval"};
IntervalAttribute intervalAttr{"bInterval"};
LengthAttribute lengthAttr{"bLength"};
EpAttributesAttribute epAttributesAttr{"bmAttributes"};
EpMaxPacketSizeAttribute epMaxPacketSizeAttr{"wMaxPacketSize"};
EpTypeAttribute epTypeAttr{"type"};

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

	drvcore::registerMbusDevice(entity.id(), device);
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
}

async::result<void> bindDevice(mbus_ng::Entity entity, mbus_ng::Properties properties) {
	auto address = std::get<mbus_ng::StringItem>(properties["usb.hub_port"]);
	auto mbus_bus = std::get<mbus_ng::StringItem>(properties["usb.bus"]);
	uint64_t bus = std::stol(mbus_bus.value);
	auto parent = drvcore::getMbusDevice(std::stol(mbus_bus.value));

	assert(usbControllerMap.find(bus) != usbControllerMap.end());
	auto bus_num = usbControllerMap[bus];

	auto sysfs_name = std::to_string(bus_num) + "-" + std::to_string(std::stoi(address.value, 0, 16));

	std::cout << "POSIX: Installing USB device " << sysfs_name << " (mbus ID: " << entity.id() << ")" << std::endl;

	auto lane = (co_await entity.getRemoteLane()).unwrap();
	auto hw = protocols::usb::connect(std::move(lane));

	auto device = std::make_shared<UsbDevice>(sysfs_name, entity.id(), parent, std::move(hw));

	/* obtain the device descroptor */
	auto raw_dev_desc = (co_await device->device().deviceDescriptor()).value();
	device->descriptors.insert(device->descriptors.end(), raw_dev_desc.begin(), raw_dev_desc.end());

	device->portNum = std::stoi(address.value) + 1;
	device->busNum = bus_num;
	device->speed = std::get<mbus_ng::StringItem>(properties["usb.speed"]).value;

	auto config_val = (co_await device->device().currentConfigurationValue()).value();
	std::string raw_desc;

	/* obtain the tree of configuration descriptors and its subdescriptors */
	auto devdesc = reinterpret_cast<protocols::usb::DeviceDescriptor *>(raw_dev_desc.data());
	protocols::usb::ConfigDescriptor cfgdesc;
	for(size_t i = 0; i < devdesc->numConfigs; i++) {
		auto raw_descs = (co_await device->device().configurationDescriptor(i)).value();
		cfgdesc = DescriptorBase::from_vec<protocols::usb::ConfigDescriptor>(raw_descs);

		if(cfgdesc.configValue == config_val)
			raw_desc = raw_descs;
		device->descriptors.insert(device->descriptors.end(), raw_descs.begin(), raw_descs.end());
	}

	protocols::usb::walkConfiguration(raw_desc, [&] (int type, size_t, void *descriptor, const auto &info) {
		if(type == protocols::usb::descriptor_type::configuration) {
			auto desc = reinterpret_cast<protocols::usb::ConfigDescriptor *>(descriptor);
			device->maxPower = desc->maxPower * 2;
			device->numInterfaces = desc->numInterfaces;

			if(info.configNumber == config_val) {
				device->bmAttributes = desc->bmAttributes;
			}
		} else if(type == protocols::usb::descriptor_type::interface) {
			auto desc = reinterpret_cast<protocols::usb::InterfaceDescriptor *>(descriptor);

			auto if_sysfs_name = std::format("{}:{}.{}", sysfs_name, *info.configNumber, desc->interfaceNumber);
			auto interface = std::make_shared<UsbInterface>(if_sysfs_name, entity.id(), device);

			interface->interfaceClass = desc->interfaceClass;
			interface->interfaceSubClass = desc->interfaceSubClass;
			interface->interfaceProtocol = desc->interfaceProtocol;
			interface->alternateSetting = desc->alternateSetting;
			interface->interfaceNumber = desc->interfaceNumber;
			interface->endpointCount = desc->numEndpoints;
			interface->descriptors = device->descriptors;

			device->interfaces.push_back(interface);
		} else if(type == protocols::usb::descriptor_type::endpoint) {
			auto desc = reinterpret_cast<protocols::usb::EndpointDescriptor *>(descriptor);

			auto ep_sysfs_name = std::format("ep_{:02x}", desc->endpointAddress & 0x8F);
			auto ep = std::make_shared<UsbEndpoint>(ep_sysfs_name, entity.id(), device->interfaces.back());
			ep->endpointAddress = desc->endpointAddress;
			ep->interval = desc->interval;
			ep->attributes = desc->attributes;
			ep->maxPacketSize = desc->maxPacketSize;
			device->interfaces.back()->endpoints.push_back(ep);
		}
	});

	drvcore::registerMbusDevice(entity.id(), device);
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
		interface->createSymlink("subsystem", sysfsSubsystem->object());

		for(auto ep : interface->endpoints) {
			ep->addObject();

			ep->realizeAttribute(&endpointAddressAttr);
			ep->realizeAttribute(&prettyIntervalAttr);
			ep->realizeAttribute(&intervalAttr);
			ep->realizeAttribute(&lengthAttr);
			ep->realizeAttribute(&epAttributesAttr);
			ep->realizeAttribute(&epMaxPacketSizeAttr);
			ep->realizeAttribute(&epTypeAttr);
		}
	}

	// TODO: Call realizeAttribute *before* installing the device.
	device->realizeAttribute(&vendorAttr);
	device->realizeAttribute(&deviceAttr);
	device->realizeAttribute(&deviceClassAttr);
	device->realizeAttribute(&deviceSubClassAttr);
	device->realizeAttribute(&deviceProtocolAttr);
	device->realizeAttribute(&bcdDeviceAttr);

	device->realizeAttribute(&manufacturerNameAttr);
	device->realizeAttribute(&productNameAttr);
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

	device->createSymlink("subsystem", sysfsSubsystem->object());

	auto ep = std::make_shared<UsbEndpoint>("ep_00", entity.id(), device);
	ep->addObject();
	ep->realizeAttribute(&endpointAddressAttr);
	ep->realizeAttribute(&prettyIntervalAttr);
	ep->realizeAttribute(&intervalAttr);
	ep->realizeAttribute(&lengthAttr);
	ep->realizeAttribute(&epAttributesAttr);
	ep->realizeAttribute(&epMaxPacketSizeAttr);
	ep->realizeAttribute(&epTypeAttr);
}

std::unordered_map<std::string, std::shared_ptr<drvcore::BusDriver>> interface_driver_list;

std::shared_ptr<drvcore::BusDriver> getInterfaceDriver(std::string name) {
	if(interface_driver_list.contains(name)) {
		return interface_driver_list.at(name);
	}

	if(name == "cdc_ncm") {
		auto ncmDriver = std::make_shared<CdcNcmDriver>(sysfsSubsystem, name);
		ncmDriver->addObject();
		interface_driver_list.insert({name, ncmDriver});
	} else if(name == "cdc_mbim") {
		auto mbimDriver = std::make_shared<CdcMbimDriver>(sysfsSubsystem, name);
		mbimDriver->addObject();
		interface_driver_list.insert({name, mbimDriver});
	} else if(name == "cdc_ether") {
		auto cdcEtherDriver = std::make_shared<CdcEtherDriver>(sysfsSubsystem, name);
		cdcEtherDriver->addObject();
		interface_driver_list.insert({name, cdcEtherDriver});
	} else {
		assert(!"unsupported USB interface driver");
	}

	return interface_driver_list.at(name);
}

async::detached observeDeviceChildren(mbus_ng::EntityId deviceId) {
	auto filter = mbus_ng::EqualsFilter{"drvcore.mbus-parent", std::to_string(deviceId)};
	auto enumerator = mbus_ng::Instance::global().enumerate(filter);

	while(true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);

			auto device = ({
				auto parent_id = std::get<mbus_ng::StringItem>(event.properties["drvcore.mbus-parent"]).value;
				drvcore::getMbusDevice(mbus_ng::EntityId{std::stoi(parent_id)});
			});
			assert(device);

			auto if_drivers = event.properties.find("usb.interface_drivers");
			if(if_drivers != event.properties.end()) {
				auto drivers_list = std::get<mbus_ng::ArrayItem>(if_drivers->second).items;

				for(auto &driver_info : drivers_list) {
					auto info = std::get<mbus_ng::ArrayItem>(driver_info).items;
					auto if_num = std::get<mbus_ng::StringItem>(info.at(0)).value;
					auto driver_name = std::get<mbus_ng::StringItem>(info.at(1)).value;

					auto dev = std::static_pointer_cast<UsbDevice>(device);
					auto config_val = (co_await dev->device().currentConfigurationValue()).value();
					auto dev_if = std::find_if(
						dev->interfaces.begin(), dev->interfaces.end(),
						[&](const auto &intf) {
							return std::format("{}.{}", config_val, intf->interfaceNumber) == if_num;
						}
					);

					if(dev_if != dev->interfaces.end() && !dev_if->get()->driver) {
						dev_if->get()->driver = getInterfaceDriver(driver_name);
						dev_if->get()->createSymlink("driver", dev_if->get()->driver);
					}
				}
			}
		}
	}
}

async::detached observeDevicesOnController(mbus_ng::EntityId controllerId) {
	auto usbDeviceFilter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter{"unix.subsystem", "usb"},
		mbus_ng::EqualsFilter{"usb.type", "device"},
		mbus_ng::EqualsFilter{"usb.bus", std::to_string(controllerId)},
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(usbDeviceFilter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);

			if (event.type == mbus_ng::EnumerationEvent::Type::created) {
				observeDeviceChildren(entity.id());
				co_await bindDevice(std::move(entity), std::move(event.properties));
			} else {
				continue;
			}
		}
	}
}

async::detached run() {
	sysfsSubsystem = std::make_shared<drvcore::BusSubsystem>("usb");
	netSubsystem = new drvcore::ClassSubsystem{"net"};
	usbmiscSubsystem = new drvcore::ClassSubsystem{"usbmisc"};

	auto usbControllerFilter = mbus_ng::EqualsFilter{"generic.devtype", "usb-controller"};

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

async::result<std::shared_ptr<drvcore::Device>> getInterfaceDevice(std::shared_ptr<drvcore::Device> parent, mbus_ng::Properties &prop) {
	assert(parent);
	// TODO(no92): check the device type before casting instead of having it be caller-checked
	auto dev = std::static_pointer_cast<UsbDevice>(parent);
	assert(dev);
	auto if_num = std::get_if<mbus_ng::StringItem>(&prop["usb.parent-interface"]);
	assert(if_num);

	auto config_val = (co_await dev->device().currentConfigurationValue()).value();
	auto dev_if = std::find_if(
		dev->interfaces.begin(), dev->interfaces.end(),
		[&](const auto &intf) {
			return std::format("{}.{}", config_val, intf->interfaceNumber) == if_num->value;
		}
	);

	if(dev_if != dev->interfaces.end())
		co_return dev_if->get()->shared_from_this();

	co_return {};
}

} // namespace usb_subsystem
