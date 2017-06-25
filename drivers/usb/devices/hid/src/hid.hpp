
#ifndef HID_HID_HPP
#define HID_HID_HPP

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/await.hpp>
#include <libevbackend.hpp>

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
	HidDevice();
	void parseReportDescriptor(Device device, uint8_t* p, uint8_t* limit);
	void translateToLinux(int page, int id, int value);
	cofiber::no_future runHidDevice(Device device);

	std::vector<Field> fields;
	std::vector<Element> elements;

private:
	std::shared_ptr<libevbackend::EventDevice> _eventDev;
};

#endif // HID_HID_HPP

