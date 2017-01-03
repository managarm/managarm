
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <vector>

#include <async/result.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/mbus/client.hpp>

#include "common.hpp"
#include "hw.pb.h"
#include "pci.hpp"

std::vector<std::shared_ptr<PciDevice>> allDevices;

COFIBER_ROUTINE(cofiber::no_future, handleDevice(std::shared_ptr<PciDevice> device,
		helix::UniqueLane p), ([device, lane = std::move(p)] {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::hw::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
	
		if(req.req_type() == managarm::hw::CntReqType::GET_PCI_INFO) {
			helix::SendBuffer send_resp;

			managarm::hw::SvrResponse resp;
			for(size_t k = 0; k < 6; k++) {
				managarm::hw::PciBar &msg = *resp.add_bars();
				if(device->bars[k].type == PciDevice::kBarIo) {
					msg.set_io_type(managarm::hw::IoType::PORT);
					msg.set_address(device->bars[k].address);
					msg.set_length(device->bars[k].length);
				}else if(device->bars[k].type == PciDevice::kBarMemory) {
					msg.set_io_type(managarm::hw::IoType::MEMORY);
					msg.set_address(device->bars[k].address);
					msg.set_length(device->bars[k].length);
				}else{
					assert(device->bars[k].type == PciDevice::kBarNone);
					msg.set_io_type(managarm::hw::IoType::NO_BAR);
				}
			}
			resp.set_error(managarm::hw::Errors::SUCCESS);
		
			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_BAR) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_bar;
	
			auto index = req.index();
			if(device->bars[index].type != PciDevice::kBarIo 
					&& device->bars[index].type != PciDevice::kBarMemory)
				throw std::runtime_error("bar is type: NO_BAR!\n");
			
			managarm::hw::SvrResponse resp;
			resp.set_error(managarm::hw::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_bar, helix::BorrowedDescriptor(device->bars[index].handle)));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_bar.error());
		}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_IRQ) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_irq;

			managarm::hw::SvrResponse resp;
			resp.set_error(managarm::hw::Errors::SUCCESS);
			
			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_irq, helix::BorrowedDescriptor(device->interrupt)));

			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_irq.error());
		}else if(req.req_type() == managarm::hw::CntReqType::LOAD_PCI_SPACE) {
			helix::SendBuffer send_resp;

			// TODO: Support other access sizes.
			// TODO: Perform some sanity checks on the offset.
			auto word = readPciHalf(device->bus, device->slot, device->function, req.offset());

			managarm::hw::SvrResponse resp;
			resp.set_error(managarm::hw::Errors::SUCCESS);
			resp.set_word(word);
		
			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else {
			helix::SendBuffer send_resp;

			managarm::hw::SvrResponse resp;
			resp.set_error(managarm::hw::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}));

COFIBER_ROUTINE(cofiber::no_future, registerDevice(std::shared_ptr<PciDevice> device), ([=] {
	char vendor[5], device_id[5], revision[3];
	sprintf(vendor, "%.4x", device->vendor);
	sprintf(device_id, "%.4x", device->deviceId);
	sprintf(revision, "%.2x", device->revision);
	
	char class_code[3], sub_class[3], interface[3];
	sprintf(class_code, "%.2x", device->classCode);
	sprintf(sub_class, "%.2x", device->subClass);
	sprintf(interface, "%.2x", device->interface);

	std::unordered_map<std::string, std::string> descriptor {
		{ "pci-vendor", vendor },
		{ "pci-device", device_id },
		{ "pci-revision", revision },
		{ "pci-class", class_code },
		{ "pci-subclass", sub_class },
		{ "pci-interface", interface }
	};

	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
	
	char name[9];
	sprintf(name, "%.2x.%.2x.%.1x", device->bus, device->slot, device->function);
	auto object = COFIBER_AWAIT root.createObject(name, descriptor,
			[=] (mbus::AnyQuery query) -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		std::cout << "pre handleDevice()" << std::endl;
		handleDevice(device, std::move(local_lane));
		std::cout << "post handleDevice()" << std::endl;

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});
	std::cout << "Created object " << name << std::endl;
}))

// --------------------------------------------------------
// Discovery functionality
// --------------------------------------------------------

size_t computeBarLength(uint32_t mask) {
	static_assert(sizeof(int) == 4, "Need long builtins");
	
	assert(mask);
	size_t length_bits = __builtin_ctz(mask);
	size_t decoded_bits = 32 - __builtin_clz(mask);
	assert(__builtin_popcount(mask) == decoded_bits - length_bits);

	return size_t(1) << length_bits;
}

void checkPciFunction(uint32_t bus, uint32_t slot, uint32_t function) {
	uint16_t vendor = readPciHalf(bus, slot, function, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	uint8_t header_type = readPciByte(bus, slot, function, kPciHeaderType);
	if((header_type & 0x7F) == 0) {
		std::cout << "    Function " << function << ": Device" << std::endl;
	}else if((header_type & 0x7F) == 1) {
		uint8_t secondary = readPciByte(bus, slot, function, kPciBridgeSecondary);
		std::cout << "    Function " << function
				<< ": PCI-to-PCI bridge to bus " << (int)secondary << std::endl;
	}else{
		std::cout << "    Function " << function
				<< ": Unexpected PCI header type " << (header_type & 0x7F) << std::endl;
	}

	uint16_t device_id = readPciHalf(bus, slot, function, kPciDevice);
	uint8_t revision = readPciByte(bus, slot, function, kPciRevision);
	std::cout << "        Vendor: 0x" << std::hex << vendor << std::dec
			<< ", device ID: 0x" << std::hex << device_id << std::dec
			<< ", revision: " << (int)revision << std::endl;
	
	uint8_t class_code = readPciByte(bus, slot, function, kPciClassCode);
	uint8_t sub_class = readPciByte(bus, slot, function, kPciSubClass);
	uint8_t interface = readPciByte(bus, slot, function, kPciInterface);
	std::cout << "        Class: " << (int)class_code
			<< ", subclass: " << (int)sub_class << ", interface: " << (int)interface << std::endl;
	
	if((header_type & 0x7F) == 0) {
		uint16_t subsystem_vendor = readPciHalf(bus, slot, function, kPciRegularSubsystemVendor);
		uint16_t subsystem_device = readPciHalf(bus, slot, function, kPciRegularSubsystemDevice);
		std::cout << "        Subsystem vendor: 0x" << std::hex << subsystem_vendor << std::dec
				<< ", device: 0x" << std::hex << subsystem_device << std::dec << std::endl;

		if(readPciHalf(bus, slot, function, kPciStatus) & 0x10) {
			// NOTE: the bottom two bits of each capability offset must be masked
			uint8_t offset = readPciByte(bus, slot, function, kPciRegularCapabilities) & 0xFC;
			while(offset != 0) {
				uint8_t capability = readPciByte(bus, slot, function, offset);
				uint8_t successor = readPciByte(bus, slot, function, offset + 1);
				
				std::cout << "        Capability 0x"
						<< std::hex << (int)capability << std::dec << std::endl;
				if(capability == 0x09) {
					uint8_t size = readPciByte(bus, slot, function, offset + 2);

					std::cout << "            Bytes: ";
					for(size_t i = 2; i < size; i++) {
						uint8_t byte = readPciByte(bus, slot, function, offset + i);
						std::cout << (i > 2 ? ", " : "") << std::hex << byte << std::dec;
					}
					std::cout << std::endl;
				}

				offset = successor & 0xFC;
			}
		}


		auto device = std::make_shared<PciDevice>(bus, slot, function,
				vendor, device_id, revision, class_code, sub_class, interface);
		
		// determine the BARs
		for(int i = 0; i < 6; i++) {
			uint32_t offset = kPciRegularBar0 + i * 4;
			uint32_t bar = readPciWord(bus, slot, function, offset);
			if(bar == 0)
				continue;
			
			if((bar & 1) != 0) {
				uintptr_t address = bar & 0xFFFFFFFC;
				
				// write all 1s to the BAR and read it back to determine this its length.
				writePciWord(bus, slot, function, offset, 0xFFFFFFFF);
				uint32_t mask = readPciWord(bus, slot, function, offset) & 0xFFFFFFFC;
				writePciWord(bus, slot, function, offset, bar);
				auto length = computeBarLength(mask);

				std::vector<uintptr_t> ports;
				for(uintptr_t offset = 0; offset < length; offset++)
					ports.push_back(address + offset);

				device->bars[i].type = PciDevice::kBarIo;
				device->bars[i].address = address;
				device->bars[i].length = length;
				HEL_CHECK(helAccessIo(ports.data(), ports.size(), &device->bars[i].handle));

				std::cout << "        I/O space BAR #" << i
						<< " at 0x" << std::hex << address << std::dec
						<< ", length: " << length << " ports" << std::endl;
			}else if(((bar >> 1) & 3) == 0) {
				uint32_t address = bar & 0xFFFFFFF0;
				
				// write all 1s to the BAR and read it back to determine this its length
				writePciWord(bus, slot, function, offset, 0xFFFFFFFF);
				uint32_t mask = readPciWord(bus, slot, function, offset) & 0xFFFFFFF0;
				writePciWord(bus, slot, function, offset, bar);
				auto length = computeBarLength(mask);
				
/*				device->bars[i].type = PciDevice::kBarMemory;
				device->bars[i].address = address;
				device->bars[i].length = length;
				HEL_CHECK(helAccessPhysical(address, length, &device->bars[i].handle));
*/
				std::cout << "        32-bit memory BAR #" << i
						<< " at 0x" << std::hex << address << std::dec
						<< ", length: " << length << " bytes" << std::endl;
			}else if(((bar >> 1) & 3) == 2) {
				assert(i < 5); // otherwise there is no next bar.
				std::cout << "        64-bit memory BAR ignored for now!" << std::endl;
				i++;
			}else{
				assert(!"Unexpected BAR type");
			}
		}

		// determine the interrupt line
		uint8_t line_number = readPciByte(bus, slot, function, kPciRegularInterruptLine);
		std::cout << "        Interrupt line: " << (int)line_number << std::endl;
		HEL_CHECK(helAccessIrq(line_number, &device->interrupt));

		registerDevice(device);

		allDevices.push_back(device);
	}
}

void checkPciDevice(uint32_t bus, uint32_t slot) {
	uint16_t vendor = readPciHalf(bus, slot, 0, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	std::cout << "Bus: " << bus << ", slot " << slot << std::endl;
	
	uint8_t header_type = readPciByte(bus, slot, 0, kPciHeaderType);
	if((header_type & 0x80) != 0) {
		for(uint32_t function = 0; function < 8; function++)
			checkPciFunction(bus, slot, function);
	}else{
		checkPciFunction(bus, slot, 0);
	}
}

void checkPciBus(uint32_t bus) {
	for(uint32_t slot = 0; slot < 32; slot++)
		checkPciDevice(bus, slot);
}

void pciDiscover() {
	uintptr_t ports[] = { 0xCF8, 0xCF9, 0xCFA, 0xCFB, 0xCFC, 0xCFD, 0xCFE, 0xCFF };
	HelHandle io_handle;
	HEL_CHECK(helAccessIo(ports, 8, &io_handle));
	HEL_CHECK(helEnableIo(io_handle));

	checkPciBus(0);
}

