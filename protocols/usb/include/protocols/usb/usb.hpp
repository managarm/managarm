
#ifndef LIBUSB_USB_HPP
#define LIBUSB_USB_HPP

#include <experimental/optional>
#include <assert.h>

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
	SetupPacket() = default;

	uint8_t type;
	uint8_t request;
	uint16_t value;
	uint16_t index;
	uint16_t length;
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
		
		// TODO: Move non-standard features to some other location.
		getReport = 0x01
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
		report = 0x22
	};
}

struct DescriptorBase {
	uint8_t length;
	uint8_t descriptorType;
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
	uint8_t interfaceProtocoll;
	uint8_t iInterface;
};

struct [[ gnu::packed ]] EndpointDescriptor : public DescriptorBase {
	uint8_t endpointAddress;
	uint8_t attributes;
	uint16_t maxPacketSize;
	uint8_t interval;
};

template<typename F>
void walkConfiguration(std::string buffer, F functor) {
	struct {
		std::experimental::optional<int> configNumber;
		std::experimental::optional<int> interfaceNumber;
		std::experimental::optional<int> interfaceAlternative;
		std::experimental::optional<int> endpointNumber;
		std::experimental::optional<bool> endpointIn;
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
			info.interfaceNumber = std::experimental::nullopt;
			info.interfaceAlternative = std::experimental::nullopt;
			info.endpointNumber = std::experimental::nullopt;
			info.endpointIn = std::experimental::nullopt;
		}else if(base->descriptorType == descriptor_type::interface) {
			auto desc = (InterfaceDescriptor *)base;
			assert(desc->length == sizeof(InterfaceDescriptor));

			info.interfaceNumber = desc->interfaceNumber;
			info.interfaceAlternative = desc->alternateSetting;
			info.endpointNumber = std::experimental::nullopt;
			info.endpointIn = std::experimental::nullopt;
		}else if(base->descriptorType == descriptor_type::endpoint) {
			auto desc = (EndpointDescriptor *)base;
			assert(desc->length == sizeof(EndpointDescriptor));
		
			info.endpointNumber = desc->endpointAddress & 0x0F;
			info.endpointIn = desc->endpointAddress & 0x80;
		}

		functor(base->descriptorType, base->length, base, info);
	}
}

// ------------------------------------------------------------------
// Contiguous Allocator.
// ------------------------------------------------------------------

#include <frigg/memory.hpp>

struct ContiguousPolicy {
public:
	uintptr_t map(size_t length);
	void unmap(uintptr_t address, size_t length);
};

using ContiguousAllocator = frigg::SlabAllocator<
	ContiguousPolicy,
	frigg::TicketLock
>;

extern ContiguousAllocator contiguousAllocator;

#endif // LIBUSB_USB_HPP

