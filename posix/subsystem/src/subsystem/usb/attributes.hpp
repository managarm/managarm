#pragma once

#include "../../drvcore.hpp"

namespace usb_subsystem {

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

struct DeviceClassAttribute : sysfs::Attribute {
	DeviceClassAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct DeviceSubClassAttribute : sysfs::Attribute {
	DeviceSubClassAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct DeviceProtocolAttribute : sysfs::Attribute {
	DeviceProtocolAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct BcdDeviceAttribute : sysfs::Attribute {
	BcdDeviceAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct VersionAttribute : sysfs::Attribute {
	VersionAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct SpeedAttribute : sysfs::Attribute {
	SpeedAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct DeviceMaxPowerAttribute : sysfs::Attribute {
	DeviceMaxPowerAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct ControllerMaxPowerAttribute : sysfs::Attribute {
	ControllerMaxPowerAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct MaxChildAttribute : sysfs::Attribute {
	MaxChildAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct NumInterfacesAttribute : sysfs::Attribute {
	NumInterfacesAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct BusNumAttribute : sysfs::Attribute {
	BusNumAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct DevNumAttribute : sysfs::Attribute {
	DevNumAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct DescriptorsAttribute : sysfs::Attribute {
	DescriptorsAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct RxLanesAttribute : sysfs::Attribute {
	RxLanesAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct TxLanesAttribute : sysfs::Attribute {
	TxLanesAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct ConfigValueAttribute : sysfs::Attribute {
	ConfigValueAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct MaxPacketSize0Attribute : sysfs::Attribute {
	MaxPacketSize0Attribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct ConfigurationAttribute : sysfs::Attribute {
	ConfigurationAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct BmAttributesAttribute : sysfs::Attribute {
	BmAttributesAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct NumConfigurationsAttribute : sysfs::Attribute {
	NumConfigurationsAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct InterfaceClassAttribute : sysfs::Attribute {
	InterfaceClassAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct InterfaceSubClassAttribute : sysfs::Attribute {
	InterfaceSubClassAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct InterfaceProtocolAttribute : sysfs::Attribute {
	InterfaceProtocolAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct AlternateSettingAttribute : sysfs::Attribute {
	AlternateSettingAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct InterfaceNumberAttribute : sysfs::Attribute {
	InterfaceNumberAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

struct EndpointNumAttribute : sysfs::Attribute {
	EndpointNumAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<std::string> show(sysfs::Object *object) override;
};

} // namespace usb_subsystem
