#pragma once

#include <cstdint>
#include <protocols/usb/client.hpp>

namespace pages {
	constexpr int genericDesktop = 0x01;
	constexpr int simulationControls = 0x02;
	constexpr int vrControls = 0x03;
	constexpr int sportControls = 0x04;
	constexpr int gameControls = 0x05;
	constexpr int genericDevice = 0x06;
	constexpr int keyboard = 0x07;
	constexpr int led = 0x08;
	constexpr int button = 0x09;
	constexpr int ordinal = 0x0A;
	constexpr int telephony = 0x0B;
	constexpr int consumer = 0x0C;
	constexpr int digitizers = 0x0D;
	constexpr int unicode = 0x10;
	constexpr int alphanumericalDisplay = 0x14;
	constexpr int medicalInstrument = 0x40;
	constexpr int firstVendorDefined = 0xFF00;
	constexpr int lastVendorDefined = 0xFFFF;

} // namespace pages

namespace usage {

namespace genericDesktop {
	constexpr int x = 0x30;
	constexpr int y = 0x31;
	constexpr int wheel = 0x38;
} // namespace genericDesktop

} // namespace usage

namespace item {
	constexpr uint32_t variable = (1 << 1);
	constexpr uint32_t relative = (1 << 2);
} // namespace item

struct HidDescriptor : public protocols::usb::DescriptorBase {
	struct [[ gnu::packed ]] Entry {
		uint8_t descriptorType;
		uint16_t descriptorLength;
	};

	uint16_t hidClass;
	uint8_t countryCode;
	uint8_t numDescriptors;
	Entry entries[];
};
