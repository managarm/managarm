#pragma once

#include <async/result.hpp>
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
	Element()
	: usageId{0}, usagePage{0}, isAbsolute{false},
			inputType{-1}, inputCode{-1} { }

	uint32_t usageId;
	uint16_t usagePage;
	int32_t logicalMin;
	int32_t logicalMax;
	bool isAbsolute;

	int inputType;
	int inputCode;

	bool disabled;
};

// -----------------------------------------------------
// HidDevice.
// -----------------------------------------------------

struct HidDevice {
	HidDevice();
	void parseReportDescriptor(protocols::usb::Device device, uint8_t* p, uint8_t* limit);
	async::detached run(protocols::usb::Device device, int intf_num, int config_num);

	std::vector<Field> fields;
	std::vector<Element> elements;

private:
	std::shared_ptr<libevbackend::EventDevice> _eventDev;
};
