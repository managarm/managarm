
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
	int bitSize;
	int dataMin;
	int dataMax;
	bool isSigned;
	int arraySize;
};

// -----------------------------------------------------
// Elements.
// -----------------------------------------------------

struct Element {
	uint32_t usageId;
	uint16_t usagePage;
	bool isAbsolute;
};
	
// -----------------------------------------------------
// HidDevice.
// -----------------------------------------------------

struct HidDevice {
	void parseReportDescriptor(Device device, uint8_t* p, uint8_t* limit);
	cofiber::no_future runHidDevice(Device device);

	std::vector<Field> fields;
	std::vector<Element> elements;
};

#endif // HID_HID_HPP

