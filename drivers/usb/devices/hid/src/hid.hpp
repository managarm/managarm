#pragma once

#include <async/result.hpp>
#include <protocols/usb/client.hpp>
#include <libevbackend.hpp>

enum class CollectionType {
	Root,
	Collection,
};

struct Collection;

struct Hierarchy {
	Hierarchy(Hierarchy *parent)
	: parent_{std::move(parent)} {

	}

	~Hierarchy();

	Hierarchy(const Hierarchy &) = delete;
	Hierarchy &operator= (const Hierarchy &) = delete;

	virtual CollectionType type() = 0;

	Hierarchy *parent() {
		return parent_;
	}

	std::vector<Collection *> children;

private:
	Hierarchy *parent_ = {};
};

struct Collection final : public Hierarchy {
	Collection(Hierarchy *parent, uint8_t type, uint32_t usage)
	: Hierarchy{std::move(parent)}, type_{type}, usageId_{usage} {

	}

	CollectionType type() override {
		return CollectionType::Collection;
	}

	uint8_t collectionType() {
		return type_;
	}

	uint32_t usageId() {
		return usageId_;
	}

private:
	uint8_t type_;
	uint32_t usageId_;
};

struct Root final : public Hierarchy {
	Root() : Hierarchy{{}} {

	}

	CollectionType type() override {
		return CollectionType::Root;
	}
};

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
	unsigned int bitSize;
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
	Hierarchy *parent;

	uint32_t usageId;
	uint16_t usagePage;
	int32_t logicalMin;
	int32_t logicalMax;
	uint8_t reportId;
	bool isAbsolute;

	int inputType;
	int inputCode;

	size_t elementNum;
};

// -----------------------------------------------------
// Handler.
// -----------------------------------------------------

// Used for implementing specific Handler for devices that need special logic, like touchscreens.
struct Handler {
	// called for handling a HID report with the given elements and values
	virtual void handleReport(std::shared_ptr<libevbackend::EventDevice> eventDev,
		std::vector<Element> &elements, std::vector<std::pair<bool, int32_t>> &values) = 0;

	// called for setting up evdev events for that Element
	virtual void setupElement(std::shared_ptr<libevbackend::EventDevice> eventDev, Element *) = 0;
};

// -----------------------------------------------------
// HidDevice.
// -----------------------------------------------------

struct HidDevice {
	HidDevice() = default;
	void parseReportDescriptor(protocols::usb::Device device, uint8_t* p, uint8_t* limit);
	async::detached run(protocols::usb::Device device, int intf_num, int config_num);

	std::pair<uint16_t, uint16_t> getDeviceId() {
		return {_vendorId, _deviceId};
	}

	std::unordered_map<uint8_t, std::vector<Field>> fields{
		{0, {}}
	};

	std::unordered_map<uint8_t, std::vector<Element>> elements{
		{0, {}}
	};

	bool usesReportIds = false;

private:
	std::shared_ptr<libevbackend::EventDevice> _eventDev;

	uint16_t _vendorId = 0xFFFF;
	uint16_t _deviceId = 0xFFFF;
};
