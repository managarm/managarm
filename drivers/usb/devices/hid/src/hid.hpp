
#ifndef HID_HID_HPP
#define HID_HID_HPP

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/await.hpp>

// -----------------------------------------------------
// Fields.
// -----------------------------------------------------

struct Field {
	int bitOffset;
	int bitSize;
	uint16_t usagePage;
	uint16_t usageId;
};

// -----------------------------------------------------
// HidDevice.
// -----------------------------------------------------

struct HidDevice {
	async::result<void> parseReportDescriptor(Device device, int index, int length, int intf_number);
	cofiber::no_future runHidDevice(Device device);

	std::vector<Field> fields;
};

#endif // HID_HID_HPP

