
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <deque>
#include <experimental/optional>

#include <cofiber.hpp>

#include "usb.hpp"
#include "api.hpp"
#include "hid.hpp"

// -----------------------------------------------------
// Fields.
// -----------------------------------------------------

struct Field {
	int bitOffset;
	int bitSize;
	uint16_t usagePage;
	uint16_t usageId;
};

std::vector<uint32_t> parse(std::vector<Field> fields, uint8_t *report) {
	std::vector<uint32_t> values;
	for(Field &f : fields) {
		int b = f.bitOffset / 8;
		uint32_t raw = uint32_t(report[b]) | (uint32_t(report[b + 1]) << 8)
				| (uint32_t(report[b + 2]) << 16) | (uint32_t(report[b + 3]) << 24);
		uint32_t mask = (uint32_t(1) << f.bitSize) - 1;
		values.push_back((raw >> (f.bitOffset % 8)) & mask);
	}
	return values;
}

std::vector<Field> fields;

uint32_t fetch(uint8_t *&p, void *limit, int n = 1) {
	uint32_t x = 0;
	for(int i = 0; i < n; i++) {
		x = (x << 8) | *p++;
		assert(p <= limit);
	}
	return x;
}

COFIBER_ROUTINE(cofiber::future<void>, parseReportDescriptor(Device device, int index), [=] () {
	printf("entered parseReportDescriptor\n");
	size_t length = 52;
	auto buffer = (uint8_t *)contiguousAllocator.allocate(length);
	COFIBER_AWAIT device.transfer(ControlTransfer(kXferToHost,
			kDestInterface, kStandard, SetupPacket::kGetDescriptor,
			(kDescriptorReport << 8) | index, 0, buffer, length));

	int bit_offset = 0;

	std::experimental::optional<int> report_count;
	std::experimental::optional<int> report_size;
	std::experimental::optional<uint16_t> usage_page;
	std::deque<uint32_t> usage;
	std::experimental::optional<uint32_t> usage_min;
	std::experimental::optional<uint32_t> usage_max;

	uint8_t *p = buffer;
	uint8_t *limit = buffer + length;
	while(p < limit) {
		uint8_t token = fetch(p, limit);
		int size = (token & 0x03) == 3 ? 4 : (token & 0x03);
		uint32_t data = fetch(p, limit, size);
		switch(token & 0xFC) {
		// Main items
		case 0xC0:
			printf("End Collection: 0x%x\n", data);
			break;

		case 0xA0:
			printf("Collection: 0x%x\n", data);
			usage.clear();
			usage_min = std::experimental::nullopt;
			usage_max = std::experimental::nullopt;
			break;

		case 0x80:
			printf("Input: 0x%x\n", data);
			if(!report_size || !report_count)
				throw std::runtime_error("Missing Report Size/Count");
				
			if(!usage_min != !usage_max)
				throw std::runtime_error("Usage Minimum without Usage Maximum or visa versa");
			
			if(!usage.empty() && (usage_min || usage_max))
				throw std::runtime_error("Usage and Usage Mnimum/Maximum specified");

			if(usage.empty() && !usage_min && !usage_max) {
				// this field is just padding
				bit_offset += (*report_size) * (*report_count);
			}else{
				for(auto i = 0; i < *report_count; i++) {
					uint16_t actual_id;
					if(!usage.empty()) {
						actual_id = usage.front();
						usage.pop_front();
					}else{
						actual_id = *usage_min + i;
					}

					Field field;
					field.bitOffset = bit_offset;
					field.bitSize = *report_size;
					field.usagePage = *usage_page;
					field.usageId = actual_id;
					fields.push_back(field);
					
					bit_offset += *report_size;
				}

				usage.clear();
				usage_min = std::experimental::nullopt;
				usage_max = std::experimental::nullopt;
			}
			break;

		// Global items
		case 0x94:
			printf("Report Count: 0x%x\n", data);
			report_count = data;
			break;
		
		case 0x74:
			printf("Report Size: 0x%x\n", data);
			report_size = data;
			break;
		
		case 0x24:
			printf("Logical Maximum: 0x%x\n", data);
			break;
		
		case 0x14:
			printf("Logical Minimum: 0x%x\n", data);
			break;
		
		case 0x04:
			printf("Usage Page: 0x%x\n", data);
			usage_page = data;
			break;

		// Local items
		case 0x28:
			printf("Usage Maximum: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			usage_max = data;
			break;
		
		case 0x18:
			printf("Usage Minimum: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			usage_min = data;
			break;
			
		case 0x08:
			printf("Usage: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			usage.push_back(data);
			break;

		default:
			printf("Unexpected token: 0x%x\n", token & 0xFC);
			abort();
		}
	}
	
/*	size_t rep_length = (bit_offset + 7) / 8;
	auto rep_buffer = (uint8_t *)contiguousAllocator.allocate(rep_length);
	COFIBER_AWAIT controller->transfer(ControlTransfer(device_state, 0, kXferToHost,
			kDestInterface, kClass, SetupPacket::kGetReport, 0x01 << 8, 0,
			rep_buffer, rep_length));

	auto values = parse(fields, rep_buffer);
	int counter = 0;
	for(uint32_t val : values) {
		printf("value %d: %x\n", counter, val);
		counter++;
	}

	for(auto f : fields) {
		printf("usagePage: %x\n", f.usagePage);
		printf("    usageId: %x\n", f.usageId);
	}*/

	COFIBER_RETURN();
})

COFIBER_ROUTINE(cofiber::no_future, runHidDevice(Device device), [=] () {
	printf("entered runHidDevice\n");
	auto descriptor = COFIBER_AWAIT device.configurationDescriptor();

	std::experimental::optional<int> config_number;
	std::experimental::optional<int> intf_number;
	std::experimental::optional<int> in_endp_number;
	std::experimental::optional<int> report_desc_index;

	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		printf("type: %i\n", type);
		if(type == kDescriptorConfig) {
			assert(!config_number);
			config_number = info.configNumber.value();
			
			auto desc = (ConfigDescriptor *)p;
			printf("Config Descriptor: \n");
			printf("    value: %i\n", desc->configValue);
		}else if(type == kDescriptorInterface) {
			assert(!intf_number);
			intf_number = info.interfaceNumber.value();
			
			auto desc = (InterfaceDescriptor *)p;
			printf("Interface Descriptor: \n");
			printf("    class: %i\n", desc->interfaceClass);
			printf("    sub class: %i\n", desc->interfaceSubClass);
			printf("    protocoll: %i\n", desc->interfaceProtocoll);
		}else if(type == kDescriptorHid) {
			auto desc = (HidDescriptor *)p;
			assert(desc->length == sizeof(HidDescriptor) + (desc->numDescriptors * sizeof(HidDescriptor::Entry)));
			
			assert(info.interfaceNumber);
			
			for(size_t i = 0; i < desc->numDescriptors; i++) {
				assert(desc->entries[i].descriptorType == kDescriptorReport);
				assert(!report_desc_index);
				report_desc_index = 0;
			}
		}else if(type == kDescriptorEndpoint) {
			assert(!in_endp_number);
			in_endp_number = info.endpointNumber.value();
		}else{
			printf("Unexpected descriptor type: %d!\n", type);
		}
	});

	COFIBER_AWAIT parseReportDescriptor(device, report_desc_index.value());
	printf("exit parseReportDescriptor\n");
	
	auto config = COFIBER_AWAIT device.useConfiguration(config_number.value());
	auto intf = COFIBER_AWAIT config.useInterface(intf_number.value(), 0);

	auto endp = intf.getEndpoint(PipeType::in, in_endp_number.value());
	while(true) {
		auto data = (uint8_t *)contiguousAllocator.allocate(4);
		COFIBER_AWAIT endp.transfer(InterruptTransfer(data, 4));
	
		auto values = parse(fields, data);
		int counter = 0;
		for(uint32_t val : values) {
			printf("value %d: %x\n", counter, val);
			counter++;
		}
	}
})

