
#include <deque>
#include <experimental/optional>
#include <iostream>

#include <assert.h>
#include <linux/input.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <async/result.hpp>
#include <libevbackend.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/client.hpp>

#include "spec.hpp"
#include "hid.hpp"

namespace {
	constexpr bool logDescriptorParser = false;
	constexpr bool logUnknownCodes = false;
	constexpr bool logFields = false;
	constexpr bool logRawPackets = false;
	constexpr bool logFieldValues = false;
	constexpr bool logInputCodes = false;
}

void setupInputTranslation(Element *element) {
	auto setInput = [&] (int type, int code) {
		element->inputType = type;
		element->inputCode = code;
	};

	if(element->usagePage == pages::genericDesktop) {
		// TODO: Distinguish between absolute and relative controls.
		if(element->isAbsolute) {
			switch(element->usageId) {
				case 0x30: setInput(EV_ABS, ABS_X); break;
				case 0x31: setInput(EV_ABS, ABS_Y); break;
				case 0x38: setInput(EV_ABS, ABS_WHEEL); break;
				default:
					if(logUnknownCodes)
						std::cout << "usb-hid: Unknown usage " << element->usageId
								<< " in Generic Desktop Page" << std::endl;
			}
		}else{
			switch(element->usageId) {
				case 0x30: setInput(EV_REL, REL_X); break;
				case 0x31: setInput(EV_REL, REL_Y); break;
				case 0x38: setInput(EV_REL, REL_WHEEL); break;
				default:
					if(logUnknownCodes)
						std::cout << "usb-hid: Unknown usage " << element->usageId
								<< " in Generic Desktop Page" << std::endl;
			}
		}
	}else if(element->usagePage == pages::keyboard) {
//		assert(element->isAbsolute);
		switch(element->usageId) {
			case 0x04: setInput(EV_KEY, KEY_A); break;
			case 0x05: setInput(EV_KEY, KEY_B); break;
			case 0x06: setInput(EV_KEY, KEY_C); break;
			case 0x07: setInput(EV_KEY, KEY_D); break;
			case 0x08: setInput(EV_KEY, KEY_E); break;
			case 0x09: setInput(EV_KEY, KEY_F); break;
			case 0x0A: setInput(EV_KEY, KEY_G); break;
			case 0x0B: setInput(EV_KEY, KEY_H); break;
			case 0x0C: setInput(EV_KEY, KEY_I); break;
			case 0x0D: setInput(EV_KEY, KEY_J); break;
			case 0x0E: setInput(EV_KEY, KEY_K); break;
			case 0x0F: setInput(EV_KEY, KEY_L); break;
			case 0x10: setInput(EV_KEY, KEY_M); break;
			case 0x11: setInput(EV_KEY, KEY_N); break;
			case 0x12: setInput(EV_KEY, KEY_O); break;
			case 0x13: setInput(EV_KEY, KEY_P); break;
			case 0x14: setInput(EV_KEY, KEY_Q); break;
			case 0x15: setInput(EV_KEY, KEY_R); break;
			case 0x16: setInput(EV_KEY, KEY_S); break;
			case 0x17: setInput(EV_KEY, KEY_T); break;
			case 0x18: setInput(EV_KEY, KEY_U); break;
			case 0x19: setInput(EV_KEY, KEY_V); break;
			case 0x1A: setInput(EV_KEY, KEY_W); break;
			case 0x1B: setInput(EV_KEY, KEY_X); break;
			case 0x1C: setInput(EV_KEY, KEY_Y); break;
			case 0x1D: setInput(EV_KEY, KEY_Z); break;
			case 0x1E: setInput(EV_KEY, KEY_1); break;
			case 0x1F: setInput(EV_KEY, KEY_2); break;
			case 0x20: setInput(EV_KEY, KEY_3); break;
			case 0x21: setInput(EV_KEY, KEY_4); break;
			case 0x22: setInput(EV_KEY, KEY_5); break;
			case 0x23: setInput(EV_KEY, KEY_6); break;
			case 0x24: setInput(EV_KEY, KEY_7); break;
			case 0x25: setInput(EV_KEY, KEY_8); break;
			case 0x26: setInput(EV_KEY, KEY_9); break;
			case 0x27: setInput(EV_KEY, KEY_0); break;
			case 0x28: setInput(EV_KEY, KEY_ENTER); break;
			case 0x29: setInput(EV_KEY, KEY_ESC); break;
			case 0x2A: setInput(EV_KEY, KEY_BACKSPACE); break;
			case 0x2B: setInput(EV_KEY, KEY_TAB); break;
			case 0x2C: setInput(EV_KEY, KEY_SPACE); break;
			case 0x2D: setInput(EV_KEY, KEY_MINUS); break;
			case 0x2E: setInput(EV_KEY, KEY_EQUAL); break;
			case 0x2F: setInput(EV_KEY, KEY_LEFTBRACE); break;
			case 0x30: setInput(EV_KEY, KEY_RIGHTBRACE); break;
			case 0x31: setInput(EV_KEY, KEY_BACKSLASH); break;
			case 0x33: setInput(EV_KEY, KEY_SEMICOLON); break;
			case 0x34: setInput(EV_KEY, KEY_APOSTROPHE); break;
			case 0x35: setInput(EV_KEY, KEY_GRAVE); break;
			case 0x36: setInput(EV_KEY, KEY_COMMA); break;
			case 0x37: setInput(EV_KEY, KEY_DOT); break;
			case 0x38: setInput(EV_KEY, KEY_SLASH); break;
			case 0x3A: setInput(EV_KEY, KEY_F1); break;
			case 0x3B: setInput(EV_KEY, KEY_F2); break;
			case 0x3C: setInput(EV_KEY, KEY_F3); break;
			case 0x3D: setInput(EV_KEY, KEY_F4); break;
			case 0x3E: setInput(EV_KEY, KEY_F5); break;
			case 0x3F: setInput(EV_KEY, KEY_F6); break;
			case 0x40: setInput(EV_KEY, KEY_F7); break;
			case 0x41: setInput(EV_KEY, KEY_F8); break;
			case 0x42: setInput(EV_KEY, KEY_F9); break;
			case 0x43: setInput(EV_KEY, KEY_F10); break;
			case 0x44: setInput(EV_KEY, KEY_F11); break;
			case 0x45: setInput(EV_KEY, KEY_F12); break;
			case 0x46: setInput(EV_KEY, KEY_SYSRQ); break;
			case 0x47: setInput(EV_KEY, KEY_SCROLLLOCK); break;
			case 0x48: setInput(EV_KEY, KEY_PAUSE); break;
			case 0x49: setInput(EV_KEY, KEY_INSERT); break;
			case 0x4A: setInput(EV_KEY, KEY_HOME); break;
			case 0x4B: setInput(EV_KEY, KEY_PAGEUP); break;
			case 0x4C: setInput(EV_KEY, KEY_DELETE); break;
			case 0x4D: setInput(EV_KEY, KEY_END); break;
			case 0x4E: setInput(EV_KEY, KEY_PAGEDOWN); break;
			case 0x4F: setInput(EV_KEY, KEY_RIGHT); break;
			case 0x50: setInput(EV_KEY, KEY_LEFT); break;
			case 0x51: setInput(EV_KEY, KEY_DOWN); break;
			case 0x52: setInput(EV_KEY, KEY_UP); break;
			case 0x53: setInput(EV_KEY, KEY_NUMLOCK); break;
			case 0x54: setInput(EV_KEY, KEY_KPSLASH); break;
			case 0x55: setInput(EV_KEY, KEY_KPASTERISK); break;
			case 0x56: setInput(EV_KEY, KEY_KPMINUS); break;
			case 0x57: setInput(EV_KEY, KEY_KPPLUS); break;
			case 0x58: setInput(EV_KEY, KEY_KPENTER); break;
			case 0x59: setInput(EV_KEY, KEY_KP1); break;
			case 0x5A: setInput(EV_KEY, KEY_KP2); break;
			case 0x5B: setInput(EV_KEY, KEY_KP3); break;
			case 0x5C: setInput(EV_KEY, KEY_KP4); break;
			case 0x5D: setInput(EV_KEY, KEY_KP5); break;
			case 0x5E: setInput(EV_KEY, KEY_KP6); break;
			case 0x5F: setInput(EV_KEY, KEY_KP7); break;
			case 0x60: setInput(EV_KEY, KEY_KP8); break;
			case 0x61: setInput(EV_KEY, KEY_KP9); break;
			case 0x62: setInput(EV_KEY, KEY_KP0); break;
			case 0x63: setInput(EV_KEY, KEY_KPDOT); break;
			case 0x64: setInput(EV_KEY, KEY_102ND); break;
			case 0xE0: setInput(EV_KEY, KEY_LEFTCTRL); break;
			case 0xE1: setInput(EV_KEY, KEY_LEFTSHIFT); break;
			case 0xE2: setInput(EV_KEY, KEY_LEFTALT); break;
			case 0xE3: setInput(EV_KEY, KEY_LEFTMETA); break;
			case 0xE4: setInput(EV_KEY, KEY_RIGHTCTRL); break;
			case 0xE5: setInput(EV_KEY, KEY_RIGHTSHIFT); break;
			case 0xE6: setInput(EV_KEY, KEY_RIGHTALT); break;
			case 0xE7: setInput(EV_KEY, KEY_RIGHTMETA); break;
			default:
				if(logUnknownCodes)
					std::cout << "usb-hid: Unknown usage " << element->usageId
							<< " in Keyboard Page" << std::endl;
		}
	}else if(element->usagePage == pages::button) {
//		assert(element->isAbsolute);
		switch(element->usageId) {
			case 0x01: setInput(EV_KEY, BTN_LEFT); break;
			case 0x02: setInput(EV_KEY, BTN_RIGHT); break;
			case 0x03: setInput(EV_KEY, BTN_MIDDLE); break;
			default:
				if(logUnknownCodes)
					std::cout << "usb-hid: Unknown usage " << element->usageId
							<< " in Button Page" << std::endl;
		}
	}else{
		if(logUnknownCodes)
			std::cout << "usb-hid: Unkown usage page " << element->usagePage << std::endl;
	}
}

int32_t signExtend(uint32_t x, int bits) {
	assert(bits > 0);
	auto m = uint32_t(1) << (bits - 1);
	return (x ^ m) - m;
}

void interpret(const std::vector<Field> &fields, uint8_t *report, size_t size,
		std::vector<std::pair<bool, int32_t>> &values) {
	int k = 0; // Offset of the value that we're generating.

	unsigned int bit_offset = 0;
	auto fetch = [&] (unsigned int bit_size, bool is_signed) -> int32_t {
		assert(bit_offset + bit_size <= size * 8);

		unsigned int b = bit_offset / 8;
		assert(b < size);
		uint32_t word = uint32_t(report[b]);
		if(b + 1 < size)
			word |= uint32_t(report[b + 1]) << 8;
		if(b + 2 < size)
			word |= uint32_t(report[b + 2]) << 16;
		if(b + 3 < size)
			word |= uint32_t(report[b + 3]) << 24;

		uint32_t mask = (uint32_t(1) << bit_size) - 1;
		uint32_t raw = (word >> (bit_offset % 8)) & mask;
//			std::cout << "bit_offset: " << bit_offset << ", raw: " << raw << std::endl;
		bit_offset += bit_size;

		if(is_signed) {
			return signExtend(raw, bit_size);
		}else{
			auto data = static_cast<int32_t>(raw);
			assert(data >= 0);
			return data;
		}
	};

	for(const Field &f : fields) {
		if(f.type == FieldType::padding) {
			for(int i = 0; i < f.arraySize; i++)
				fetch(f.bitSize, false);
			continue;
		}

		assert(f.bitSize <= 31);

		if(f.type == FieldType::array) {
			assert(!f.isSigned);
			for(int i = 0; i < f.dataMax - f.dataMin + 1; i++)
				values[k + i] = {true, 0};
			
			for(int i = 0; i < f.arraySize; i++) {
				auto data = fetch(f.bitSize, false);
				if(!(data >= f.dataMin && data <= f.dataMax))
					continue;

				values[k + data - f.dataMin] = {true, 1};
			}
			k += f.dataMax - f.dataMin + 1;
		}else{
			assert(f.type == FieldType::variable);
			auto data = fetch(f.bitSize, f.isSigned);
			if(data >= f.dataMin && data <= f.dataMax)
				values[k] = {true, data};
			k++;
		}
	}

	assert(bit_offset == size * 8);
}

struct LocalState {
	std::vector<uint32_t> usage;
	std::experimental::optional<uint32_t> usageMin;
	std::experimental::optional<uint32_t> usageMax;
};

struct GlobalState {
	std::experimental::optional<uint16_t> usagePage;
	std::experimental::optional<std::pair<int32_t, uint32_t>> logicalMin;
	std::experimental::optional<std::pair<int32_t, uint32_t>> logicalMax;
	std::experimental::optional<unsigned int> reportSize;
	std::experimental::optional<unsigned int> reportCount;
	std::experimental::optional<int> physicalMin;
	std::experimental::optional<int> physicalMax;
};

HidDevice::HidDevice() {
	_eventDev = std::make_shared<libevbackend::EventDevice>();
}

void HidDevice::parseReportDescriptor(Device device, uint8_t *p, uint8_t* limit) {
	LocalState local;
	GlobalState global;
	
	auto generateFields = [&] (bool array, bool relative) {
		if(!global.reportSize || !global.reportCount)
			throw std::runtime_error("Missing Report Size/Count");
			
		if(!local.usageMin != !local.usageMax)
			throw std::runtime_error("Usage Minimum without Usage Maximum or visa versa");
		
		if(!local.usage.empty() && (local.usageMin || local.usageMax))
			throw std::runtime_error("Usage and Usage Mnimum/Maximum specified");
			
		if(local.usage.empty() && !local.usageMin && !local.usageMax) {
			Field field;
			field.type = FieldType::padding;
			field.bitSize = global.reportSize.value();
			field.arraySize = global.reportCount.value();
			fields.push_back(field);
		}else if(!array) {
			for(unsigned int i = 0; i < global.reportCount.value(); i++) {
				uint16_t actual_id;
				if(local.usage.empty()) {
					actual_id = local.usageMin.value() + i;
				}else{
					assert(local.usage.size() == global.reportCount.value());
					actual_id = local.usage[i];
				}
				
				if(!global.logicalMin || !global.logicalMax)
					throw std::runtime_error("logicalMin or logicalMax not set");
				
				Field field;
				field.type = FieldType::variable;
				field.bitSize = global.reportSize.value();
				if(global.logicalMin.value().first < 0) {
					field.isSigned = true;
					field.dataMin = global.logicalMin.value().first;
					field.dataMax = global.logicalMax.value().first;
				}else {
					field.isSigned = false;
					field.dataMin = global.logicalMin.value().second;
					field.dataMax = global.logicalMax.value().second;
				}
				fields.push_back(field);

				Element element;
				element.usageId = actual_id;
				element.usagePage = global.usagePage.value();
				element.logicalMin = field.dataMin;
				element.logicalMax = field.dataMax;
				element.isAbsolute = !relative;
				elements.push_back(element);
			}
		}else{
			assert(!relative);

			if(!global.logicalMin || !global.logicalMax)
				throw std::runtime_error("logicalMin or logicalMax not set");
			
			if(global.logicalMin.value().first < 0) {
				if(global.logicalMin.value().first > global.logicalMax.value().first)
					throw std::runtime_error("signed: logicalMin > logicalMax");
			}else{
				if(global.logicalMin.value().second > global.logicalMax.value().second)
					throw std::runtime_error("unsigned: logicalMin > logicalMax");
			}

			if(!local.usageMin)
				throw std::runtime_error("usageMin not set");

			Field field;
			field.type = FieldType::array;
			field.bitSize = global.reportSize.value();
			if(global.logicalMin.value().first < 0) {
				field.isSigned = true;
				field.dataMin = global.logicalMin.value().first;
				field.dataMax = global.logicalMax.value().first;
			}else {
				field.isSigned = false;
				field.dataMin = global.logicalMin.value().second;
				field.dataMax = global.logicalMax.value().second;
			}
			field.arraySize = global.reportCount.value();
			fields.push_back(field);

			for(uint32_t i = 0; i < local.usageMax.value() - local.usageMin.value() + 1; i++) {
				Element element;
				element.usageId = local.usageMin.value() + i;
				element.usagePage = global.usagePage.value();
				element.logicalMin = 0;
				element.logicalMax = 1;
				elements.push_back(element);
			}
		}
	};
	
	if(logDescriptorParser)
		printf("usb-hid: Parsing report descriptor:\n");

	auto fetch = [&] (int n) -> uint32_t {
		assert(p + n <= limit);
		uint32_t x = 0;
		for(int i = 0; i < n; i++)
			x |= (*p++ << (i * 8));
		return x;
	};

	while(p < limit) {
		uint8_t token = fetch(1);
		int size = 0;
		if(token & 0x03)
			size = (1 << ((token & 0x03) - 1));
		uint32_t data = fetch(size);
		switch(token & 0xFC) {
		// Main items
		case 0xC0:
			if(logDescriptorParser)
				printf("usb-hid:     End Collection: 0x%x\n", data);
			break;

		case 0xA0:
			if(logDescriptorParser)
				printf("usb-hid:     Collection: 0x%x\n", data);
			local = LocalState();
			break;

		case 0x80:
			if(logDescriptorParser)
				printf("usb-hid:     Input: 0x%x\n", data);
			generateFields(!(data & item::variable), data & item::relative);
			local = LocalState();
			break;

		case 0x90:
			if(logDescriptorParser)
				printf("usb-hid:     Output: 0x%x\n", data);
			if(!global.reportSize || !global.reportCount)
				throw std::runtime_error("Missing Report Size/Count");
				
			if(!local.usageMin != !local.usageMax)
				throw std::runtime_error("Usage Minimum without Usage Maximum or visa versa");
			
			if(!local.usage.empty() && (local.usageMin || local.usageMax))
				throw std::runtime_error("Usage and Usage Mnimum/Maximum specified");
			
			local = LocalState();
			break;

		// Global items
		case 0x94:
			if(logDescriptorParser)
				printf("usb-hid:     Report Count: 0x%x\n", data);
			global.reportCount = data;
			break;
		
		case 0x74:
			if(logDescriptorParser)
				printf("usb-hid:     Report Size: 0x%x\n", data);
			global.reportSize = data;
			break;
		
		case 0x44:
			if(logDescriptorParser)
				printf("usb-hid:     Physical Maximum: 0x%x\n", data);
			global.physicalMax = data;
			break;
	
		case 0x34:
			if(logDescriptorParser)
				printf("usb-hid:     Physical Minimum: 0x%x\n", data);
			global.physicalMin = data;
			break;

		case 0x24:
			assert(size > 0);
			global.logicalMax = std::make_pair(signExtend(data, size * 8), data);
			if(logDescriptorParser)
				printf("usb-hid:     Logical Maximum: signed: %d, unsigned: %d\n",
						global.logicalMax.value().first,
						global.logicalMax.value().second);
			break;
		
		case 0x14:
			assert(size > 0);
			global.logicalMin = std::make_pair(signExtend(data, size * 8), data);
			if(logDescriptorParser)
				printf("usb-hid:     Logical Minimum: signed: %d, unsigned: %d\n",
						global.logicalMin.value().first,
						global.logicalMin.value().second);
			break;
		
		case 0x04:
			if(logDescriptorParser)
				printf("usb-hid:     Usage Page: 0x%x\n", data);
			global.usagePage = data;
			break;

		// Local items
		case 0x28:
			if(logDescriptorParser)
				printf("usb-hid:     Usage Maximum: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			local.usageMax = data;
			break;
		
		case 0x18:
			if(logDescriptorParser)
				printf("usb-hid:     Usage Minimum: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			local.usageMin = data;
			break;
			
		case 0x08:
			if(logDescriptorParser)
				printf("usb-hid:     Usage: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			local.usage.push_back(data);
			break;

		default:
			printf("Unexpected token: 0x%x\n", token & 0xFC);
			abort();
		}
	}
}

async::detached HidDevice::run(Device device, int config_num, int intf_num) {
	auto descriptor = co_await device.configurationDescriptor();

	std::vector<size_t> report_descs;
	std::experimental::optional<int> in_endp_number;
	size_t in_endp_pktsize;

//	std::cout << "usb-hid: Device configuration:" << std::endl;
	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
//		std::cout << "    Descriptor: " << type << std::endl;
		if(type == descriptor_type::hid) {	
			if(info.configNumber.value() != config_num
					|| info.interfaceNumber.value() != intf_num)
				return;

			auto desc = static_cast<HidDescriptor *>(p);
			assert(desc->length == sizeof(HidDescriptor)
					+ (desc->numDescriptors * sizeof(HidDescriptor::Entry)));
			
			for(size_t i = 0; i < desc->numDescriptors; i++) {
				assert(desc->entries[i].descriptorType == descriptor_type::report);
				report_descs.push_back(desc->entries[i].descriptorLength);
			}
		}else if(type == descriptor_type::endpoint) {
			if(info.configNumber.value() != config_num
					|| info.interfaceNumber.value() != intf_num)
				return;

			auto desc = static_cast<EndpointDescriptor *>(p);
			assert(!in_endp_number);

			in_endp_number = info.endpointNumber.value();
			in_endp_pktsize = desc->maxPacketSize;
		}
	});

	std::cout << "usb-hid: Using endpoint number " << in_endp_number.value() << std::endl;

	// Parse all report descriptors.
	std::cout << "usb-hid: Parsing report descriptor" << std::endl;
	for(size_t i = 0; i < report_descs.size(); i++) {
		arch::dma_object<SetupPacket> get_descriptor{device.setupPool()};
		get_descriptor->type = setup_type::targetInterface | setup_type::byStandard
				| setup_type::toHost;
		get_descriptor->request = request_type::getDescriptor;
		get_descriptor->value = (descriptor_type::report << 8) | i;
		get_descriptor->index = intf_num;
		get_descriptor->length = report_descs[i];

		arch::dma_buffer buffer{device.bufferPool(), report_descs[i]};

		co_await device.transfer(ControlTransfer{kXferToHost,
				get_descriptor, buffer});
		
		auto p = reinterpret_cast<uint8_t *>(buffer.data());
		auto limit = reinterpret_cast<uint8_t *>(buffer.data()) + report_descs[i];
		parseReportDescriptor(device, p, limit);
	}

	// Report supported input codes to the evdev core.
	for(size_t i = 0; i < elements.size(); i++) {
		auto element = &elements[i];
		setupInputTranslation(element);
		if(element->inputType < 0)
			continue;
		if(element->inputType == EV_ABS)
			_eventDev->setAbsoluteDetails(element->inputCode,
					element->logicalMin, element->logicalMax);
		_eventDev->enableEvent(element->inputType, element->inputCode);
	}

	if(logFields)
		for(size_t i = 0; i < fields.size(); i++) {
			std::cout << "Field " << i << ": [" << fields[i].arraySize
					<< "]. Bit size: " << fields[i].bitSize
					<< ", signed: " << fields[i].isSigned << std::endl;
		}

	// Create an mbus object for the device.
	auto root = co_await mbus::Instance::global().getRoot();
	
	mbus::Properties mbus_descriptor{
		{"unix.subsystem", mbus::StringItem{"input"}}
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		libevbackend::serveDevice(_eventDev, std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});

	co_await root.createObject("input_hid", mbus_descriptor, std::move(handler));
	
	auto config = co_await device.useConfiguration(config_num);
	auto intf = co_await config.useInterface(intf_num, 0);
	auto endp = co_await intf.getEndpoint(PipeType::in, in_endp_number.value());

	// Read reports from the USB device.
	std::cout << "usb-hid: Entering report loop" << std::endl;

	std::vector<std::pair<bool, int32_t>> values;
	values.resize(elements.size());
	while(true) {
//		std::cout << "usb-hid: Requesting new report" << std::endl;
		arch::dma_buffer report{device.bufferPool(), in_endp_pktsize};
		InterruptTransfer transfer{XferFlags::kXferToHost, report};
		transfer.allowShortPackets = true;
		auto length = co_await endp.transfer(transfer);

		// Some devices (e.g. bochs) send empty packets instead of NAKs.
		if(!length)
			continue;
		
		if(logRawPackets) {
			std::cout << "usb-hid: Report size: " << length
					<< " (packet size is " << in_endp_pktsize << ")" << std::endl;
			std::cout << "usb-hid: Packet:";
			std::cout << std::hex;
			for(size_t i = 0; i < 4; i++)
				std::cout << " " << (int)reinterpret_cast<uint8_t *>(report.data())[i];
			std::cout << std::dec << std::endl;
		}

		std::fill(values.begin(), values.end(), std::pair<bool, int32_t>{false, 0});
		interpret(fields, reinterpret_cast<uint8_t *>(report.data()),
				length, values);
	
		if(logFieldValues) {
			for(size_t i = 0; i < values.size(); i++)
				if(values[i].first)
					std::cout << "usagePage: " << elements[i].usagePage
							<< ", usageId: 0x" << std::hex << elements[i].usageId << std::dec
							<< ", value: " << values[i].second << std::endl;
			std::cout << std::endl;
		}

		if(logInputCodes)
			std::cout << "Reporting input event" << std::endl;
		for(size_t i = 0; i < values.size(); i++) {
			auto element = &elements[i];
			if(element->inputType < 0)
				continue;
			if(!values[i].first)
				continue;
			if(logInputCodes)
				std::cout << "    inputType: " << element->inputType
						<< ", inputCode: " << element->inputCode
						<< ", value: " << values[i].second << std::endl;
			_eventDev->emitEvent(element->inputType, element->inputCode, values[i].second);
		}
		_eventDev->emitEvent(EV_SYN, SYN_REPORT, 0);
		_eventDev->notify();
	}
}

async::detached bindDevice(mbus::Entity entity) {
	auto lane = helix::UniqueLane(co_await entity.bind());
	auto device = protocols::usb::connect(std::move(lane));

	auto descriptor = co_await device.configurationDescriptor();
	std::experimental::optional<int> config_number;
	std::experimental::optional<int> intf_number;
	std::experimental::optional<int> intf_alternative;
	
	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		if(type == descriptor_type::configuration) {
			assert(!config_number);
			config_number = info.configNumber.value();
		}else if(type == descriptor_type::interface) {
			auto desc = reinterpret_cast<InterfaceDescriptor *>(p);
			if(desc->interfaceClass != 3)
				return;
	
			if(intf_number) {
				std::cout << "usb-hid: Ignoring secondary HID interface: "
						<< info.interfaceNumber.value()
						<< ", alternative: " << info.interfaceAlternative.value() << std::endl;
				return;
			}

			intf_number = info.interfaceNumber.value();
			intf_alternative = info.interfaceAlternative.value();
		}
	});
	
	if(!intf_number)
		co_return;
	std::cout << "usb-hid: Detected HID device. "
			"Interface: " << intf_number.value()
			<< ", alternative: " << intf_alternative.value() << std::endl;

	HidDevice* hid_device = new HidDevice();
	hid_device->run(device, config_number.value(), intf_number.value());
}

async::detached observeDevices() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("usb.type", "device"),
		mbus::EqualsFilter("usb.class", "00")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		std::cout << "usb-hid: Detected USB device" << std::endl;
		bindDevice(std::move(entity));
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("usb-hid: Starting driver\n");

//	HEL_CHECK(helSetPriority(kHelThisThread, 2));

	{
		async::queue_scope scope{helix::globalQueue()};
		observeDevices();
	}

	helix::globalQueue()->run();
	
	return 0;
}

