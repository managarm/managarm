#pragma once

#include <arch/bits.hpp>
#include <assert.h>
#include <cstdint>
#include <string>
#include <optional>

namespace protocols::usb {

namespace setup_type {
	enum : uint8_t {
		// The first 5 bits specify the target of the request.
		targetMask = 0x1F,
		targetDevice = 0x00,
		targetInterface = 0x01,
		targetEndpoint = 0x02,
		targetOther = 0x03,

		// The next 2 bits determine the document that specifies the request.
		specificationMask = 0x60,
		byStandard = 0x00,
		byClass = 0x20,
		byVendor = 0x40,

		// The last bit specifies the transfer direction.
		directionMask = 0x80,
		toDevice = 0x00,
		toHost = 0x80
	};
}

// Alignment makes sure that a packet doesnt cross a page boundary
struct alignas(8) SetupPacket {
	uint8_t type = 0;
	uint8_t request = 0;
	uint16_t value = 0;
	uint16_t index = 0;
	uint16_t length = 0;
};
static_assert(sizeof(SetupPacket) == 8, "Bad SetupPacket size");

namespace request_type {
	enum : uint8_t {
		getStatus = 0x00,
		clearFeature = 0x01,
		setFeature = 0x03,
		setAddress = 0x05,
		getDescriptor = 0x06,
		setDescriptor = 0x07,
		getConfig = 0x08,
		setConfig = 0x09,
		setInterface = 0x0B,

		// TODO: Move non-standard features to some other location.
		getReport = 0x01
	};
}

namespace features {
	enum : uint8_t {
		endpointHalt = 0x00,
		deviceRemoteWakeup = 0x01,
		testMode = 0x02,
	};
}

namespace descriptor_type {
	enum : uint16_t {
		device = 0x01,
		configuration = 0x02,
		string = 0x03,
		interface = 0x04,
		endpoint = 0x05,

		// TODO: Put non-standard descriptors somewhere else.
		hid = 0x21,
		report = 0x22,
		cs_interface = 0x24,
		cs_endpoint = 0x25,
	};
}

namespace usb_class {
	enum : uint8_t {
		per_interface = 0x00,
		cdc = 0x02,
		hid = 0x03,
		mass_storage = 0x08,
		cdc_data = 0x0A,
		vendor_specific = 0xFF,
	};
} // namespace usb_class

namespace cdc_subclass {
	enum : uint8_t {
		reserved = 0x00,
		ethernet = 0x06,
		ncm = 0x0D,
		mbim = 0x0E,
		vendor_specific = 0xFF,
	};
} // namespace cdc_subclass

struct DescriptorBase {
	uint8_t length;
	uint8_t descriptorType;
};

struct [[gnu::packed]] StringDescriptor : public DescriptorBase {
	char16_t data[0];
};

struct DeviceDescriptor : public DescriptorBase {
	uint16_t bcdUsb;
	uint8_t deviceClass;
	uint8_t deviceSubclass;
	uint8_t deviceProtocol;
	uint8_t maxPacketSize;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t manufacturer;
	uint8_t product;
	uint8_t serialNumber;
	uint8_t numConfigs;
};
//FIXME: remove alignas
//static_assert(sizeof(DeviceDescriptor) == 18, "Bad DeviceDescriptor size");

struct [[ gnu::packed ]] ConfigDescriptor : public DescriptorBase {
	uint16_t totalLength;
	uint8_t numInterfaces;
	uint8_t configValue;
	uint8_t iConfig;
	uint8_t bmAttributes;
	uint8_t maxPower;
};

struct InterfaceDescriptor : public DescriptorBase {
	uint8_t interfaceNumber;
	uint8_t alternateSetting;
	uint8_t numEndpoints;
	uint8_t interfaceClass;
	uint8_t interfaceSubClass;
	uint8_t interfaceProtocol;
	uint8_t iInterface;
};

/* CDC 1.1 5.2.3.1 */
struct [[gnu::packed]] CdcDescriptor : public DescriptorBase {
	enum class CdcSubType : uint8_t {
		Header = 0x00,
		CallManagement = 0x01,
		AbstractControl = 0x02,
		Union = 0x06,
		EthernetNetworking = 0x0F,
		Ncm = 0x1A,
	};

	CdcSubType subtype;
};

/* CDC 1.1 5.2.3.1 */
struct [[gnu::packed]] CdcHeader : public CdcDescriptor {
	uint16_t bcdCDC;
};

/* CDC 1.1 6.3 */
struct [[gnu::packed]] CdcNotificationHeader {
	enum class Notification : uint8_t {
		NETWORK_CONNECTION = 0x00,
		RESPONSE_AVAILABLE = 0x01,
		AUX_JACK_HOOK_STATE = 0x08,
		RING_DETECT = 0x09,
		SERIAL_STATE = 0x20,
		CALL_STATE_CHANGE = 0x28,
		LINE_STATE_CHANGE = 0x29,
		CONNECTION_SPEED_CHANGE = 0x2A,
	};

	uint8_t bmRequestType;
	Notification bNotificationCode;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
};

/* CDC 1.1 6.3.8 */
struct [[gnu::packed]] CdcConnectionSpeedChange {
	uint32_t DlBitRate;
	uint32_t UlBitRate;
};

/* CDC 1.1 5.2.3.2 */
struct [[gnu::packed]] CdcCallManagement : public CdcDescriptor {
	uint8_t bmCapabilities;
	uint8_t bDataInterface;
};

/* CDC 1.1 5.2.3.3 */
struct [[gnu::packed]] CdcAbstractControl : public CdcDescriptor {
	uint8_t bmCapabilities;
};

/* CDC 1.1 5.2.3.8 */
struct [[gnu::packed]] CdcUnion : public CdcDescriptor {
	uint8_t bControlInterface;
	uint8_t bSubordinateInterface[1];
};

/* CDC 1.1 5.2.3.16 */
struct [[gnu::packed]] CdcEthernetNetworking : public CdcDescriptor {
	uint8_t iMACAddress;
	uint32_t bmEthernetStatistics;
	uint16_t wMaxSegmentSize;
	uint16_t wNumberMCFilters;
	uint8_t bNumberPowerFilters;
};

/* NCM 1.0 5.2.1 */
struct [[gnu::packed]] CdcNcm : public CdcDescriptor {
	uint16_t bcdNcmVersion;
	arch::bit_value<uint8_t> bmNetworkCapabilities;
};

struct [[ gnu::packed ]] EndpointDescriptor : public DescriptorBase {
	uint8_t endpointAddress;
	uint8_t attributes;
	uint16_t maxPacketSize;
	uint8_t interval;
};

enum class EndpointType {
	control = 0,
	isochronous,
	bulk,
	interrupt
};

template<typename F>
void walkConfiguration(std::string buffer, F functor) {
	struct {
		std::optional<int> configNumber;
		std::optional<int> interfaceNumber;
		std::optional<int> interfaceAlternative;
		std::optional<int> endpointNumber;
		std::optional<bool> endpointIn;
		std::optional<EndpointType> endpointType;
	} info;

	auto p = &buffer[0];
	auto limit = &buffer[0] + buffer.size();
	while(p < limit) {
		auto base = (DescriptorBase *)p;
		p += base->length;

		if(base->descriptorType == descriptor_type::configuration) {
			auto desc = (ConfigDescriptor *)base;
			assert(desc->length == sizeof(ConfigDescriptor));

			info.configNumber = desc->configValue;
			info.interfaceNumber = std::nullopt;
			info.interfaceAlternative = std::nullopt;
			info.endpointNumber = std::nullopt;
			info.endpointIn = std::nullopt;
		}else if(base->descriptorType == descriptor_type::interface) {
			auto desc = (InterfaceDescriptor *)base;
			assert(desc->length == sizeof(InterfaceDescriptor));

			info.interfaceNumber = desc->interfaceNumber;
			info.interfaceAlternative = desc->alternateSetting;
			info.endpointNumber = std::nullopt;
			info.endpointIn = std::nullopt;
		}else if(base->descriptorType == descriptor_type::endpoint) {
			auto desc = (EndpointDescriptor *)base;
			assert(desc->length == sizeof(EndpointDescriptor));

			info.endpointNumber = desc->endpointAddress & 0x0F;
			info.endpointIn = desc->endpointAddress & 0x80;
			info.endpointType = static_cast<EndpointType>(desc->attributes & 0x03);
		}

		functor(base->descriptorType, base->length, base, info);
	}
}

} // namespace protocols::usb
