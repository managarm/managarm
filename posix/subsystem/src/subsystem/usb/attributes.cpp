#include <format>

#include "attributes.hpp"
#include "devices.hpp"

namespace usb_subsystem {

async::result<frg::expected<Error, std::string>> VendorAttribute::show(sysfs::Object *object) {
	char buffer[6]; // The format is 1234\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 6, "%.4x\n", device->desc()->idVendor);
	co_return std::string{buffer};
}

async::result<frg::expected<Error, std::string>> DeviceAttribute::show(sysfs::Object *object) {
	char buffer[6]; // The format is 1234\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 6, "%.4x\n", device->desc()->idProduct);
	co_return std::string{buffer};
}

async::result<frg::expected<Error, std::string>> DeviceClassAttribute::show(sysfs::Object *object) {
	char buffer[4]; // The format is 34\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 4, "%.2x\n", device->desc()->deviceClass);
	co_return std::string{buffer};
}

async::result<frg::expected<Error, std::string>> DeviceSubClassAttribute::show(sysfs::Object *object) {
	char buffer[4]; // The format is 34\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 4, "%.2x\n", device->desc()->deviceSubclass);
	co_return std::string{buffer};
}

async::result<frg::expected<Error, std::string>> DeviceProtocolAttribute::show(sysfs::Object *object) {
	char buffer[4]; // The format is 34\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 4, "%.2x\n", device->desc()->deviceProtocol);
	co_return std::string{buffer};
}

async::result<frg::expected<Error, std::string>> BcdDeviceAttribute::show(sysfs::Object *object) {
	char buffer[6]; // The format is 34\n\0.
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 6, "%.4x\n", device->desc()->bcdDevice);
	co_return std::string{buffer};
}

async::result<frg::expected<Error, std::string>> VersionAttribute::show(sysfs::Object *object) {
	char buffer[7]; // The format is " 2.00\n\0".
	auto device = static_cast<UsbBase *>(object);
	snprintf(buffer, 7, "%2x.%02x\n", device->desc()->bcdUsb >> 8, device->desc()->bcdUsb & 0xFF);
	co_return std::string{buffer};
}

async::result<frg::expected<Error, std::string>> SpeedAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return device->speed + "\n";
}

async::result<frg::expected<Error, std::string>> DeviceMaxPowerAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	co_return std::to_string(device->maxPower) + "mA\n";
}

async::result<frg::expected<Error, std::string>> ControllerMaxPowerAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbController *>(object);
	co_return std::to_string(device->maxPower) + "mA\n";
}

async::result<frg::expected<Error, std::string>> MaxChildAttribute::show(sysfs::Object *object) {
	(void) object;
	co_return "2\n";
}

async::result<frg::expected<Error, std::string>> NumInterfacesAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	char buf[4];
	snprintf(buf, 4, "%2x\n", device->numInterfaces);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> BusNumAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::to_string(device->busNum) + "\n";
}

async::result<frg::expected<Error, std::string>> DevNumAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::to_string(device->portNum) + "\n";
}

async::result<frg::expected<Error, std::string>> DescriptorsAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::string{reinterpret_cast<char *>(device->descriptors.data()), device->descriptors.size()};
}

async::result<frg::expected<Error, std::string>> RxLanesAttribute::show(sysfs::Object *object) {
	(void) object;
	co_return "1\n";
}

async::result<frg::expected<Error, std::string>> TxLanesAttribute::show(sysfs::Object *object) {
	(void) object;
	co_return "1\n";
}

async::result<frg::expected<Error, std::string>> ConfigValueAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	auto config_val = (co_await device->device().currentConfigurationValue()).value();

	co_return std::to_string(config_val) + "\n";
}

async::result<frg::expected<Error, std::string>> MaxPacketSize0Attribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);

	co_return std::to_string(device->desc()->maxPacketSize) + "\n";
}

async::result<frg::expected<Error, std::string>> ConfigurationAttribute::show(sysfs::Object *object) {
	(void) object;
	co_return "\n";
}

async::result<frg::expected<Error, std::string>> BmAttributesAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->bmAttributes);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> NumConfigurationsAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->desc()->numConfigs);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> InterfaceClassAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->interfaceClass);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> InterfaceSubClassAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->interfaceSubClass);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> InterfaceProtocolAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->interfaceProtocol);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> AlternateSettingAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%2x\n", device->alternateSetting);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> InterfaceNumberAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->interfaceNumber);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> EndpointNumAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->endpointCount);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> EndpointAddressAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->endpointAddress);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> PrettyIntervalAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	co_return std::format("{}ms\n", 0);
}

async::result<frg::expected<Error, std::string>> IntervalAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->interval);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> LengthAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->length);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> EpAttributesAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	char buf[4];
	snprintf(buf, 4, "%02x\n", device->attributes);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> EpMaxPacketSizeAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	char buf[6];
	snprintf(buf, 6, "%04x\n", device->maxPacketSize);
	co_return std::string{buf};
}

async::result<frg::expected<Error, std::string>> EpTypeAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	switch(device->attributes & 0x03) {
		case 0: co_return "Control";
		case 1: co_return "Isochronous";
		case 2: co_return "Bulk";
		case 3: co_return "Interrupt";
	}

	__builtin_unreachable();
}

} // namespace usb_subsystem
