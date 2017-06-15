
#include <deque>
#include <experimental/optional>
#include <iostream>

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/await.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/client.hpp>

#include "spec.hpp"
#include "hid.hpp"

int32_t signExtend(uint32_t x, int bits) {
	assert(bits > 0);
	auto m = uint32_t(1) << (bits - 1);
	return (x ^ m) - m;
}

void interpret(std::vector<Field> fields, uint8_t *report) {
	unsigned int bit_offset = 0;
	for(Field &f : fields) {

		auto fetch = [&] () {
			int b = bit_offset / 8;
			uint32_t raw = uint32_t(report[b]) | (uint32_t(report[b + 1]) << 8)
					| (uint32_t(report[b + 2]) << 16) | (uint32_t(report[b + 3]) << 24);
			uint32_t mask = (uint32_t(1) << f.bitSize) - 1;
			uint32_t data = (raw >> (bit_offset % 8)) & mask;
			bit_offset += f.bitSize;
			return data;
		};

		if(f.type == FieldType::padding) {
			for(int i = 0; i < f.arraySize; i++)
				fetch();
			continue;
		}

		assert(f.bitSize <= 31);

		if(f.type == FieldType::array) {
			assert(!f.isSigned);
			std::vector<int> values;
			values.resize(f.logicalMax - f.logicalMin + 1, 0);
			
			for(int i = 0; i < f.arraySize; i++) {
				auto data = fetch();
				if(!(data >= f.logicalMin && data <= f.logicalMax))
					continue;

				values[data - f.logicalMin] = 1;
			}
			for(int i = 0; i < values.size(); i++) {
				std::cout << "usagePage: " << f.usagePage << ", usageId: 0x" << std::hex
						<< (f.usageId + f.logicalMin + i) << std::dec << ", value: " << values[i] << std::endl;
			}
		}else{
			uint32_t usage;
			int32_t value;
			assert(f.type == FieldType::variable);
			auto data = fetch();
			if(f.isSigned) {
				usage = f.usageId;
				value = signExtend(data, f.bitSize);
			}else{
				usage = f.usageId;
				value = data;
			}
			if(!(value >= f.logicalMin && value <= f.logicalMax))
				continue;

			std::cout << "usagePage: " << f.usagePage << ", usageId: 0x" << std::hex
					<< usage << std::dec << ", value: " << value << std::endl;
		}
	}
	std::cout << std::endl;
}

uint32_t fetch(uint8_t *&p, void *limit, int n = 1) {
	uint32_t x = 0;
	for(int i = 0; i < n; i++) {
		x = (x << 8) | *p++;
		assert(p <= limit);
	}
	return x;
}

struct LocalState {
	std::vector<uint32_t> usage;
	std::experimental::optional<uint32_t> usageMin;
	std::experimental::optional<uint32_t> usageMax;
};

struct GlobalState {
	std::experimental::optional<uint16_t> usagePage;
	std::experimental::optional<int> logicalMin;
	std::experimental::optional<int> logicalMax;
	std::experimental::optional<int> reportSize;
	std::experimental::optional<int> reportCount;
	std::experimental::optional<int> physicalMin;
	std::experimental::optional<int> physicalMax;
};

void HidDevice::parseReportDescriptor(Device device, uint8_t *p, uint8_t* limit) {
	LocalState local;
	GlobalState global;
	
	auto generateFields = [&] (bool array) {
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
			for(int i = 0; i < global.reportCount.value(); i++) {
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
				field.usagePage = global.usagePage.value();
				field.logicalMin = global.logicalMin.value();
				field.logicalMax = global.logicalMax.value();
				field.usageId = actual_id;
				field.isSigned = global.logicalMin.value() < 0;
				
				fields.push_back(field);
			}
		}else{
			if(!global.logicalMin || !global.logicalMax)
				throw std::runtime_error("logicalMin or logicalMax not set");
			
			if(!local.usageMin)
				throw std::runtime_error("usageMin not set");

			Field field;
			field.type = FieldType::array;
			field.bitSize = global.reportSize.value();
			field.usagePage = global.usagePage.value();
			field.logicalMin = global.logicalMin.value();
			field.logicalMax = global.logicalMax.value();
			field.usageId = local.usageMin.value();
			field.isSigned = global.logicalMin.value() < 0;
			field.arraySize = global.reportCount.value();

			fields.push_back(field);
		}
	};

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
			local = LocalState();
			break;

		case 0x80:
			printf("Input: 0x%x\n", data);
			generateFields(!(data & item::variable));
			local = LocalState();
			break;

		case 0x90:
			printf("Output: 0x%x\n", data);
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
			printf("Report Count: 0x%x\n", data);
			global.reportCount = data;
			break;
		
		case 0x74:
			printf("Report Size: 0x%x\n", data);
			global.reportSize = data;
			break;
		
		case 0x44:
			printf("Physical Maximum: 0x%x\n", data);
			global.physicalMax = data;
			break;
	
		case 0x34:
			printf("Physical Minimum: 0x%x\n", data);
			global.physicalMin = data;
			break;

		case 0x24:
			assert(size > 0);
			//global.logicalMax = signExtend(data, size * 8);
			global.logicalMax = data;
			printf("Logical Maximum: %d\n", global.logicalMax.value());
			break;
		
		case 0x14:
			assert(size > 0);
			//global.logicalMin = signExtend(data, size * 8);
			global.logicalMin = data;
			printf("Logical Minimum: %d\n", global.logicalMin.value());
			break;
		
		case 0x04:
			printf("Usage Page: 0x%x\n", data);
			global.usagePage = data;
			break;

		// Local items
		case 0x28:
			printf("Usage Maximum: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			local.usageMax = data;
			break;
		
		case 0x18:
			printf("Usage Minimum: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			local.usageMin = data;
			break;
			
		case 0x08:
			printf("Usage: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			local.usage.push_back(data);
			break;

		default:
			printf("Unexpected token: 0x%x\n", token & 0xFC);
			abort();
		}
	}
}

COFIBER_ROUTINE(cofiber::no_future, HidDevice::runHidDevice(Device device), ([=] {
	auto descriptor = COFIBER_AWAIT device.configurationDescriptor();

	std::experimental::optional<int> config_number;
	std::experimental::optional<int> intf_number;
	std::experimental::optional<int> in_endp_number;
	std::experimental::optional<int> report_desc_index;
	std::experimental::optional<int> report_desc_length;

	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		if(type == descriptor_type::configuration) {
			assert(!config_number);
			config_number = info.configNumber.value();
			
			auto desc = (ConfigDescriptor *)p;
		}else if(type == descriptor_type::interface) {
			assert(!intf_number);
			intf_number = info.interfaceNumber.value();
			
			auto desc = (InterfaceDescriptor *)p;
			assert(desc->interfaceClass == 3);
		}else if(type == descriptor_type::hid) {
			auto desc = (HidDescriptor *)p;
			assert(desc->length == sizeof(HidDescriptor) + (desc->numDescriptors * sizeof(HidDescriptor::Entry)));
			
			assert(info.interfaceNumber);
			
			for(size_t i = 0; i < desc->numDescriptors; i++) {
				assert(desc->entries[i].descriptorType == descriptor_type::report);
				assert(!report_desc_index);
				report_desc_index = 0;
				report_desc_length = desc->entries[i].descriptorLength;

			}
		}else if(type == descriptor_type::endpoint) {
			assert(!in_endp_number);
			in_endp_number = info.endpointNumber.value();
		}else{
			printf("Unexpected descriptor type: %d!\n", type);
		}
	});
	
	arch::dma_object<SetupPacket> get_descriptor{device.setupPool()};
	get_descriptor->type = setup_type::targetInterface | setup_type::byStandard
			| setup_type::toHost;
	get_descriptor->request = request_type::getDescriptor;
	get_descriptor->value = (descriptor_type::report << 8) | report_desc_index.value();
	get_descriptor->index = intf_number.value();
	get_descriptor->length = report_desc_length.value();

	arch::dma_buffer buffer{device.bufferPool(), report_desc_length.value()};

	COFIBER_AWAIT device.transfer(ControlTransfer{kXferToHost,
			get_descriptor, buffer});
	
	auto p = reinterpret_cast<uint8_t *>(buffer.data());
	auto limit = reinterpret_cast<uint8_t *>(buffer.data()) + report_desc_length.value();

	parseReportDescriptor(device, p, limit);
	
	auto config = COFIBER_AWAIT device.useConfiguration(config_number.value());
	auto intf = COFIBER_AWAIT config.useInterface(intf_number.value(), 0);

	auto endp = COFIBER_AWAIT(intf.getEndpoint(PipeType::in, in_endp_number.value()));
	while(true) {
		arch::dma_buffer report{device.bufferPool(), 4};
		COFIBER_AWAIT endp.transfer(InterruptTransfer{XferFlags::kXferToHost, report});
		interpret(fields, reinterpret_cast<uint8_t *>(report.data()));
	}
}))

COFIBER_ROUTINE(cofiber::no_future, bindDevice(mbus::Entity entity), ([=] {
	auto lane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
	auto device = protocols::usb::connect(std::move(lane));
	HidDevice* hid_device = new HidDevice();
	hid_device->runHidDevice(device);
}))

COFIBER_ROUTINE(cofiber::no_future, observeDevices(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("usb.type", "device"),
		mbus::EqualsFilter("usb.class", "00")
	});
	COFIBER_AWAIT root.linkObserver(std::move(filter),
			[] (mbus::AnyEvent event) {
		if(event.type() == typeid(mbus::AttachEvent)) {
			std::cout << "uhci: Detected hid-device" << std::endl;
			bindDevice(boost::get<mbus::AttachEvent>(event).getEntity());
		}else{
			throw std::runtime_error("Unexpected device class");
		}
	});
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting hid (usb-)driver\n");

	observeDevices();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}

