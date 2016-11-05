
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

COFIBER_ROUTINE(cofiber::future<void>, parseReportDescriptor(Device device), [=] () {
	size_t length = 52;
	auto buffer = (uint8_t *)contiguousAllocator.allocate(length);
	COFIBER_AWAIT device.transfer(ControlTransfer(kXferToHost,
			kDestInterface, kStandard, SetupPacket::kGetDescriptor, 34 << 8, 0,
			buffer, length));

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
	auto config_buffer = COFIBER_AWAIT device.configurationDescriptor();

	auto p = &config_buffer[0];
	auto limit = &config_buffer[0] + config_buffer.size();
	while(p < limit) {
		auto base = (DescriptorBase *)p;
		p += base->length;

		if(base->descriptorType == kDescriptorInterface) {
			auto desc = (InterfaceDescriptor *)base;
			assert(desc->length == sizeof(InterfaceDescriptor));

			printf("Interface:\n");
			printf("   if num:%d \n", desc->interfaceNumber);	
			printf("   alternate setting:%d \n", desc->alternateSetting);	
			printf("   num endpoints:%d \n", desc->numEndpoints);	
			printf("   if class:%d \n", desc->interfaceClass);	
			printf("   if sub class:%d \n", desc->interfaceSubClass);	
			printf("   if protocoll:%d \n", desc->interfaceProtocoll);	
			printf("   if id:%d \n", desc->iInterface);	
		}else if(base->descriptorType == kDescriptorEndpoint) {
			auto desc = (EndpointDescriptor *)base;
			assert(desc->length == sizeof(EndpointDescriptor));

			printf("Endpoint:\n");
			printf("   endpoint address:%d \n", desc->endpointAddress);	
			printf("   attributes:%d \n", desc->attributes);	
			printf("   max packet size:%d \n", desc->maxPacketSize);	
			printf("   interval:%d \n", desc->interval);
		}else if(base->descriptorType == kDescriptorHid) {
			auto desc = (HidDescriptor *)base;
			assert(desc->length == sizeof(HidDescriptor) + (desc->numDescriptors * sizeof(HidDescriptor::Entry)));
			
			printf("HID:\n");
			printf("   hid class:%d \n", desc->hidClass);
			printf("   country code:%d \n", desc->countryCode);
			printf("   num descriptors:%d \n", desc->numDescriptors);
			printf("   Entries:\n");
			for(size_t entry = 0; entry < desc->numDescriptors; entry++) {
				printf("        Entry %lu:\n", entry);
				printf("        length:%d\n", desc->entries[entry].descriptorLength);
				printf("        type:%d\n", desc->entries[entry].descriptorType);
			}
		}else{
			printf("Unexpected descriptor type: %d!\n", base->descriptorType);
		}
	}

	auto config = COFIBER_AWAIT device.useConfiguration(1);
	auto intf = COFIBER_AWAIT config.useInterface(1, 0);
	COFIBER_AWAIT parseReportDescriptor(device);

	auto endp = intf.getEndpoint(PipeType::in, 1);
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

