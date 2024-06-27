#include <format>

#include "attributes.hpp"
#include "devices.hpp"

namespace usb_subsystem {

async::result<frg::expected<Error, std::string>> VendorAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::format("{:0>4x}\n", device->desc()->idVendor);
}

async::result<frg::expected<Error, std::string>> DeviceAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::format("{:0>4x}\n", device->desc()->idProduct);
}

async::result<frg::expected<Error, std::string>> DeviceClassAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::format("{:0>2x}\n", device->desc()->deviceClass);
}

async::result<frg::expected<Error, std::string>> DeviceSubClassAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::format("{:0>2x}\n", device->desc()->deviceSubclass);
}

async::result<frg::expected<Error, std::string>> DeviceProtocolAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::format("{:0>2x}\n", device->desc()->deviceProtocol);
}

async::result<frg::expected<Error, std::string>> BcdDeviceAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::format("{:0>4x}\n", device->desc()->bcdDevice);
}

async::result<frg::expected<Error, std::string>> ManufacturerNameAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	auto str = co_await device->device().getString(device->desc()->manufacturer);
	co_return std::format("{}\n", str ? str.value() : "");
}

async::result<frg::expected<Error, std::string>> ProductNameAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	auto str = co_await device->device().getString(device->desc()->product);
	co_return std::format("{}\n", str ? str.value() : "");
}

async::result<frg::expected<Error, std::string>> VersionAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::format("{:>2x}.{:0>2x}\n", device->desc()->bcdUsb >> 8, device->desc()->bcdUsb & 0xFF);
}

async::result<frg::expected<Error, std::string>> SpeedAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::format("{}\n", device->speed);
}

async::result<frg::expected<Error, std::string>> DeviceMaxPowerAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	co_return std::format("{}mA\n", device->maxPower);
}

async::result<frg::expected<Error, std::string>> ControllerMaxPowerAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbController *>(object);
	co_return std::format("{}mA\n", device->maxPower);
}

async::result<frg::expected<Error, std::string>> MaxChildAttribute::show(sysfs::Object *object) {
	(void) object;
	co_return "2\n";
}

async::result<frg::expected<Error, std::string>> NumInterfacesAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	co_return std::format("{:>2}\n", device->numInterfaces);
}

async::result<frg::expected<Error, std::string>> BusNumAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::format("{}\n", device->busNum);
}

async::result<frg::expected<Error, std::string>> DevNumAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbBase *>(object);
	co_return std::format("{}\n", device->portNum);
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
	co_return std::format("{}\n", config_val);
}

async::result<frg::expected<Error, std::string>> MaxPacketSize0Attribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	co_return std::format("{}\n", device->desc()->maxPacketSize);
}

async::result<frg::expected<Error, std::string>> ConfigurationAttribute::show(sysfs::Object *object) {
	(void) object;
	co_return "\n";
}

async::result<frg::expected<Error, std::string>> BmAttributesAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	co_return std::format("{:2x}\n", device->bmAttributes);
}

async::result<frg::expected<Error, std::string>> NumConfigurationsAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbDevice *>(object);
	co_return std::format("{}\n", device->desc()->numConfigs);
}

async::result<frg::expected<Error, std::string>> InterfaceClassAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	co_return std::format("{:0>2x}\n", device->interfaceClass);
}

async::result<frg::expected<Error, std::string>> InterfaceSubClassAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	co_return std::format("{:0>2x}\n", device->interfaceSubClass);
}

async::result<frg::expected<Error, std::string>> InterfaceProtocolAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	co_return std::format("{:0>2x}\n", device->interfaceProtocol);
}

async::result<frg::expected<Error, std::string>> AlternateSettingAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	co_return std::format("{:>2x}\n", device->alternateSetting);
}

async::result<frg::expected<Error, std::string>> InterfaceNumberAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	co_return std::format("{:0>2x}\n", device->interfaceNumber);
}

async::result<frg::expected<Error, std::string>> EndpointNumAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbInterface *>(object);
	co_return std::format("{:0>2x}\n", device->endpointCount);
}

async::result<frg::expected<Error, std::string>> EndpointAddressAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	co_return std::format("{:0>2x}\n", device->endpointAddress);
}

async::result<frg::expected<Error, std::string>> PrettyIntervalAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	(void)device;
	co_return std::format("{}ms\n", 0);
}

async::result<frg::expected<Error, std::string>> IntervalAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	co_return std::format("{:0>2x}\n", device->interval);
}

async::result<frg::expected<Error, std::string>> LengthAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	co_return std::format("{:0>2x}\n", device->length);
}

async::result<frg::expected<Error, std::string>> EpAttributesAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	co_return std::format("{:0>2x}\n", device->attributes);
}

async::result<frg::expected<Error, std::string>> EpMaxPacketSizeAttribute::show(sysfs::Object *object) {
	auto device = static_cast<UsbEndpoint *>(object);
	co_return std::format("{:0>4x}\n", device->maxPacketSize);
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
