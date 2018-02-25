
#include <deque>
#include <experimental/optional>
#include <iostream>

#include <assert.h>
#include <linux/input.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/await.hpp>
#include <libevbackend.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/client.hpp>

#include "spec.hpp"
#include "hid.hpp"

namespace {
	constexpr bool logRawValues = false;
}

int32_t signExtend(uint32_t x, int bits) {
	assert(bits > 0);
	auto m = uint32_t(1) << (bits - 1);
	return (x ^ m) - m;
}

void interpret(std::vector<Field> fields, uint8_t *report, std::vector<int> &values) {
	int k = 0;
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
			for(int i = 0; i < f.dataMax - f.dataMin + 1; i++)
				values[k + i] = 0;
			
			for(int i = 0; i < f.arraySize; i++) {
				auto data = fetch();
				if(!(data >= f.dataMin && data <= f.dataMax))
					continue;

				values[k + data - f.dataMin] = 1;
			}
			k += f.dataMax - f.dataMin + 1;
		}else{
			assert(f.type == FieldType::variable);
			auto data = fetch();
			if(f.isSigned) {
				values[k] = signExtend(data, f.bitSize);
			}else{
				values[k] = data;
			}
			k++;
		}
	}
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
	std::experimental::optional<std::pair<int32_t, uint32_t>> logicalMin;
	std::experimental::optional<std::pair<int32_t, uint32_t>> logicalMax;
	std::experimental::optional<int> reportSize;
	std::experimental::optional<int> reportCount;
	std::experimental::optional<int> physicalMin;
	std::experimental::optional<int> physicalMax;
};

HidDevice::HidDevice() {
	_eventDev = std::make_shared<libevbackend::EventDevice>();
}

void HidDevice::translateToLinux(int page, int id, int value) {
	if(page == pages::genericDesktop) {
		// TODO: Distinguish between absolute and relative controls.
		switch(id) {
			case 0x30: _eventDev->emitEvent(EV_REL, REL_X, value); break;
			case 0x31: _eventDev->emitEvent(EV_REL, REL_Y, value); break;
		}
	}else if(page == pages::keyboard) {
		switch(id) {
			case 0x04: _eventDev->emitEvent(EV_KEY, KEY_A, value); break;
			case 0x05: _eventDev->emitEvent(EV_KEY, KEY_B, value); break;
			case 0x06: _eventDev->emitEvent(EV_KEY, KEY_C, value); break;
			case 0x07: _eventDev->emitEvent(EV_KEY, KEY_D, value); break;
			case 0x08: _eventDev->emitEvent(EV_KEY, KEY_E, value); break;
			case 0x09: _eventDev->emitEvent(EV_KEY, KEY_F, value); break;
			case 0x0A: _eventDev->emitEvent(EV_KEY, KEY_G, value); break;
			case 0x0B: _eventDev->emitEvent(EV_KEY, KEY_H, value); break;
			case 0x0C: _eventDev->emitEvent(EV_KEY, KEY_I, value); break;
			case 0x0D: _eventDev->emitEvent(EV_KEY, KEY_J, value); break;
			case 0x0E: _eventDev->emitEvent(EV_KEY, KEY_K, value); break;
			case 0x0F: _eventDev->emitEvent(EV_KEY, KEY_L, value); break;
			case 0x10: _eventDev->emitEvent(EV_KEY, KEY_M, value); break;
			case 0x11: _eventDev->emitEvent(EV_KEY, KEY_N, value); break;
			case 0x12: _eventDev->emitEvent(EV_KEY, KEY_O, value); break;
			case 0x13: _eventDev->emitEvent(EV_KEY, KEY_P, value); break;
			case 0x14: _eventDev->emitEvent(EV_KEY, KEY_Q, value); break;
			case 0x15: _eventDev->emitEvent(EV_KEY, KEY_R, value); break;
			case 0x16: _eventDev->emitEvent(EV_KEY, KEY_S, value); break;
			case 0x17: _eventDev->emitEvent(EV_KEY, KEY_T, value); break;
			case 0x18: _eventDev->emitEvent(EV_KEY, KEY_U, value); break;
			case 0x19: _eventDev->emitEvent(EV_KEY, KEY_V, value); break;
			case 0x1A: _eventDev->emitEvent(EV_KEY, KEY_W, value); break;
			case 0x1B: _eventDev->emitEvent(EV_KEY, KEY_X, value); break;
			case 0x1C: _eventDev->emitEvent(EV_KEY, KEY_Y, value); break;
			case 0x1D: _eventDev->emitEvent(EV_KEY, KEY_Z, value); break;
			case 0x1E: _eventDev->emitEvent(EV_KEY, KEY_1, value); break;
			case 0x1F: _eventDev->emitEvent(EV_KEY, KEY_2, value); break;
			case 0x20: _eventDev->emitEvent(EV_KEY, KEY_3, value); break;
			case 0x21: _eventDev->emitEvent(EV_KEY, KEY_4, value); break;
			case 0x22: _eventDev->emitEvent(EV_KEY, KEY_5, value); break;
			case 0x23: _eventDev->emitEvent(EV_KEY, KEY_6, value); break;
			case 0x24: _eventDev->emitEvent(EV_KEY, KEY_7, value); break;
			case 0x25: _eventDev->emitEvent(EV_KEY, KEY_8, value); break;
			case 0x26: _eventDev->emitEvent(EV_KEY, KEY_9, value); break;
			case 0x27: _eventDev->emitEvent(EV_KEY, KEY_0, value); break;
		}
	}
}

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
				//element.isAbsolute = true;
				elements.push_back(element);
			}
		}else{
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

			for(int i = 0; i < local.usageMax.value() - local.usageMin.value() + 1; i++) {
				Element element;
				element.usageId = local.usageMin.value() + i;
				element.usagePage = global.usagePage.value();
				//element.isAbsolute = true;
				elements.push_back(element);
			}
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
			global.logicalMax = std::make_pair(signExtend(data, size * 8), data);
			printf("Logical Maximum: signed: %d, unsigned: %d\n", global.logicalMax.value().first,
					global.logicalMax.value().second);
			break;
		
		case 0x14:
			assert(size > 0);
			global.logicalMin = std::make_pair(signExtend(data, size * 8), data);
			printf("Logical Minimum: signed: %d, unsigned: %d\n", global.logicalMin.value().first,
					global.logicalMin.value().second);
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

COFIBER_ROUTINE(cofiber::no_future, HidDevice::run(Device device, int config_num,
		int intf_num), ([=] {
	auto descriptor = COFIBER_AWAIT device.configurationDescriptor();

	std::experimental::optional<int> in_endp_number;
	std::experimental::optional<int> report_desc_index;
	std::experimental::optional<int> report_desc_length;

	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		if(type == descriptor_type::hid) {
			auto desc = (HidDescriptor *)p;
			assert(desc->length == sizeof(HidDescriptor) + (desc->numDescriptors * sizeof(HidDescriptor::Entry)));
			
			assert(info.interfaceNumber);
			
			for(size_t i = 0; i < desc->numDescriptors; i++) {
				assert(desc->entries[i].descriptorType == descriptor_type::report);
				assert(!report_desc_index);
				report_desc_index = 0;
				report_desc_length = (int)desc->entries[i].descriptorLength;
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
	get_descriptor->index = intf_num;
	get_descriptor->length = report_desc_length.value();

	arch::dma_buffer buffer{device.bufferPool(), report_desc_length.value()};

	COFIBER_AWAIT device.transfer(ControlTransfer{kXferToHost,
			get_descriptor, buffer});
	
	auto p = reinterpret_cast<uint8_t *>(buffer.data());
	auto limit = reinterpret_cast<uint8_t *>(buffer.data()) + report_desc_length.value();

	parseReportDescriptor(device, p, limit);
	
	auto config = COFIBER_AWAIT device.useConfiguration(config_num);
	auto intf = COFIBER_AWAIT config.useInterface(intf_num, 0);

	auto endp = COFIBER_AWAIT(intf.getEndpoint(PipeType::in, in_endp_number.value()));

	// Create an mbus object for the device.
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
	
	mbus::Properties mbus_descriptor{
		{"unix.subsystem", mbus::StringItem{"input"}},
		{"unix.devname", mbus::StringItem{"input/event0"}}
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

	COFIBER_AWAIT root.createObject("input_hid", mbus_descriptor, std::move(handler));

	// Read reports from the USB device.
	std::vector<int> values;
	values.resize(elements.size());
	while(true) {
		arch::dma_buffer report{device.bufferPool(), 4};
		COFIBER_AWAIT endp.transfer(InterruptTransfer{XferFlags::kXferToHost, report});
		interpret(fields, reinterpret_cast<uint8_t *>(report.data()), values);
	
		if(logRawValues) {
			for(int i = 0; i < values.size(); i++)
				std::cout << "usagePage: " << elements[i].usagePage << ", usageId: 0x" << std::hex
						<< elements[i].usageId << std::dec << ", value: " << values[i] << std::endl;
			std::cout << std::endl;
		}

		for(int i = 0; i < values.size(); i++)
			translateToLinux(elements[i].usagePage, elements[i].usageId, values[i]);
		_eventDev->emitEvent(EV_SYN, SYN_REPORT, 0);
	}
}))

COFIBER_ROUTINE(cofiber::no_future, bindDevice(mbus::Entity entity), ([=] {
	auto lane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
	auto device = protocols::usb::connect(std::move(lane));

	auto descriptor = COFIBER_AWAIT device.configurationDescriptor();
	std::experimental::optional<int> config_number;
	std::experimental::optional<int> intf_number;
	std::experimental::optional<int> intf_class;
	
	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		if(type == descriptor_type::configuration) {
			assert(!config_number);
			config_number = info.configNumber.value();
		}else if(type == descriptor_type::interface) {
			assert(!intf_number);
			intf_number = info.interfaceNumber.value();
			
			auto desc = (InterfaceDescriptor *)p;
			intf_class = desc->interfaceClass;
		}
	});
	
	if(intf_class.value() != 3)
		return;

	HidDevice* hid_device = new HidDevice();
	hid_device->run(device, config_number.value(), intf_number.value());
}))

COFIBER_ROUTINE(cofiber::no_future, observeDevices(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("usb.type", "device"),
		mbus::EqualsFilter("usb.class", "00")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		std::cout << "uhci: Detected hid-device" << std::endl;
		bindDevice(std::move(entity));
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
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

