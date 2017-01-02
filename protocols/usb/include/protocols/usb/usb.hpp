
#ifndef LIBUSB_USB_HPP
#define LIBUSB_USB_HPP

#include <experimental/optional>
#include <assert.h>

enum DataDirection {
	kDirToDevice = 0,
	kDirToHost = 1
};

enum ControlRecipient {
	kDestDevice = 0,
	kDestInterface = 1,
	kDestEndpoint = 2,
	kDestOther = 3
};

enum ControlType {
	kStandard = 0,
	kClass = 1,
	kVendor = 2,
	kReserved = 3
};

enum DescriptorType {
	kDescriptorDevice = 0x01,
	kDescriptorConfig = 0x02,
	kDescriptorString = 0x03,
	kDescriptorInterface = 0x04,
	kDescriptorEndpoint = 0x05,
	kDescriptorHid = 0x21,
	kDescriptorReport = 0x22
};

// Alignment makes sure that a packet doesnt cross a page boundary
struct alignas(8) SetupPacket {
	enum Request {
		kGetStatus = 0x00,
		kClearFeature = 0x01,
		kGetReport = 0x01,
		kSetFeature = 0x03,
		kSetAddress = 0x05,
		kGetDescriptor = 0x06,
		kSetDescriptor = 0x07,
		kGetConfig = 0x08,
		kSetConfig = 0x09
	};

	static constexpr uint8_t RecipientBits = 0;
	static constexpr uint8_t TypeBits = 5;
	static constexpr uint8_t DirectionBit = 7;

	SetupPacket(DataDirection data_direction, ControlRecipient recipient, ControlType type,
			uint8_t breq, uint16_t wval, uint16_t wid, uint16_t wlen)
	: bmRequestType((uint8_t(recipient) << RecipientBits)
				| (uint8_t(type) << TypeBits)
				| (uint8_t(data_direction) << DirectionBit)),
		bRequest(breq), wValue(wval), wIndex(wid), wLength(wlen) { }

	uint8_t bmRequestType;
	uint8_t bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
};
static_assert(sizeof(SetupPacket) == 8, "Bad SetupPacket size");

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
		
		if(base->descriptorType == kDescriptorConfig) {
			auto desc = (ConfigDescriptor *)base;
			assert(desc->length == sizeof(ConfigDescriptor));

			info.configNumber = desc->configValue;
			info.interfaceNumber = std::experimental::nullopt;
			info.interfaceAlternative = std::experimental::nullopt;
			info.endpointNumber = std::experimental::nullopt;
			info.endpointIn = std::experimental::nullopt;
		}else if(base->descriptorType == kDescriptorInterface) {
			auto desc = (InterfaceDescriptor *)base;
			assert(desc->length == sizeof(InterfaceDescriptor));

			info.interfaceNumber = desc->interfaceNumber;
			info.interfaceAlternative = desc->alternateSetting;
			info.endpointNumber = std::experimental::nullopt;
			info.endpointIn = std::experimental::nullopt;
		}else if(base->descriptorType == kDescriptorEndpoint) {
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

