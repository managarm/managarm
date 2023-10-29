#include "attributes.hpp"
#include "devices.hpp"

namespace usb_subsystem {

async::result<std::string> VendorAttribute::show(sysfs::Object *object) {
	char buffer[6]; // The format is 1234\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 6, "%.4x\n", device->desc()->idVendor);
	co_return std::string{buffer};
}

async::result<std::string> DeviceAttribute::show(sysfs::Object *object) {
	char buffer[6]; // The format is 1234\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 6, "%.4x\n", device->desc()->idProduct);
	co_return std::string{buffer};
}

async::result<std::string> DeviceClassAttribute::show(sysfs::Object *object) {
	char buffer[4]; // The format is 34\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 4, "%.2x\n", device->desc()->deviceClass);
	co_return std::string{buffer};
}

async::result<std::string> DeviceSubClassAttribute::show(sysfs::Object *object) {
	char buffer[4]; // The format is 34\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 4, "%.2x\n", device->desc()->deviceSubclass);
	co_return std::string{buffer};
}

async::result<std::string> DeviceProtocolAttribute::show(sysfs::Object *object) {
	char buffer[4]; // The format is 34\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 4, "%.2x\n", device->desc()->deviceProtocol);
	co_return std::string{buffer};
}

async::result<std::string> BcdDeviceAttribute::show(sysfs::Object *object) {
	char buffer[6]; // The format is 34\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 6, "%.4x\n", device->desc()->bcdDevice);
	co_return std::string{buffer};
}

async::result<std::string> VersionAttribute::show(sysfs::Object *object) {
	char buffer[7]; // The format is " 2.00\n\0".
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 7, "%2x.%02x\n", device->desc()->bcdUsb >> 8, device->desc()->bcdUsb & 0xFF);
	co_return std::string{buffer};
}

async::result<std::string> SpeedAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return device->speed + "\n";
}

async::result<std::string> MaxPowerAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	co_return std::to_string(device->maxPower) + "mA\n";
}

async::result<std::string> MaxChildAttribute::show(sysfs::Object *object) {
	co_return "2\n";
}

async::result<std::string> NumInterfacesAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	char buf[4];
	snprintf(buf, 4, "%2x\n", device->numInterfaces);
	co_return std::string{buf};
}

async::result<std::string> BusNumAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::to_string(device->busNum) + "\n";
}

async::result<std::string> DevNumAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::to_string(device->portNum) + "\n";
}

async::result<std::string> DescriptorsAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::string{reinterpret_cast<char *>(device->descriptors.data()), device->descriptors.size()};
}

async::result<std::string> RxLanesAttribute::show(sysfs::Object *object) {
	co_return "1\n";
}

async::result<std::string> TxLanesAttribute::show(sysfs::Object *object) {
	co_return "1\n";
}

async::result<std::string> ConfigValueAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	auto config_val = (co_await device->device().currentConfigurationValue()).value();

	co_return std::to_string(config_val) + "\n";
}

async::result<std::string> MaxPacketSize0Attribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);

	co_return std::to_string(device->desc()->maxPacketSize) + "\n";
}

async::result<std::string> ConfigurationAttribute::show(sysfs::Object *object) {
	co_return "\n";
}

async::result<std::string> BmAttributesAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->bmAttributes);
	co_return std::string{buf};
}

async::result<std::string> NumConfigurationsAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->desc()->numConfigs);
	co_return std::string{buf};
}

async::result<std::string> InterfaceClassAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->interfaceClass);
	co_return std::string{buf};
}

async::result<std::string> InterfaceSubClassAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->interfaceSubClass);
	co_return std::string{buf};
}

async::result<std::string> InterfaceProtocolAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->interfaceProtocol);
	co_return std::string{buf};
}

async::result<std::string> AlternateSettingAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%2x\n", device->alternateSetting);
	co_return std::string{buf};
}

async::result<std::string> InterfaceNumberAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->interfaceNumber);
	co_return std::string{buf};
}

async::result<std::string> EndpointNumAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->endpoints);
	co_return std::string{buf};
}

} // namespace usb_subsystem
