
#ifndef HID_HID_HPP
#define HID_HID_HPP

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/await.hpp>

// -----------------------------------------------------
// Fields.
// -----------------------------------------------------

enum class FieldType {
	null,
	padding,
	variable,
	array
};

struct Field {
	FieldType type;
	int bitOffset;
	int bitSize;
	uint16_t usagePage;
	uint16_t usageId;
	int logicalMin;
	int logicalMax;
	int usageMin;
	int usageMax;
};

// -----------------------------------------------------
// HidDevice.
// -----------------------------------------------------

struct HidDevice {
	void parseReportDescriptor(Device device, uint8_t* p, uint8_t* limit);
	cofiber::no_future runHidDevice(Device device);

	std::vector<Field> fields;
};

#endif // HID_HID_HPP

