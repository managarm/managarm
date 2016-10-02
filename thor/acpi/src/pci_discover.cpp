
#include <frigg/cxx-support.hpp>
#include <frigg/glue-hel.hpp>
#include <frigg/callback.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/protobuf.hpp>
#include <frigg/chain-all.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "common.hpp"
#include "pci.hpp"
#include <mbus.frigg_pb.hpp>
#include <hw.frigg_pb.hpp>

frigg::Vector<PciDevice *, Allocator> allDevices(*allocator.unsafeGet());

// --------------------------------------------------------
// DeviceClosure
// --------------------------------------------------------

struct DeviceClosure {
	DeviceClosure(helx::Pipe pipe, PciDevice *device);

	void operator() ();

private:
	helx::Pipe pipe;
	
	PciDevice *device;
};

DeviceClosure::DeviceClosure(helx::Pipe pipe, PciDevice *device)
: pipe(frigg::move(pipe)), device(device) { }

void DeviceClosure::operator() () {
	managarm::hw::PciDevice<Allocator> response(*allocator);

	for(size_t k = 0; k < 6; k++) {
		if(device->bars[k].type == PciDevice::kBarIo) {
			managarm::hw::PciBar<Allocator> bar_response(*allocator);
			bar_response.set_io_type(managarm::hw::IoType::PORT);
			bar_response.set_address(device->bars[k].address);
			bar_response.set_length(device->bars[k].length);
			response.add_bars(frigg::move(bar_response));

			auto action = pipe.sendDescriptorResp(device->bars[k].handle, eventHub, 1, 1 + k)
			+ frigg::lift([=] (HelError error) { HEL_SOFT_CHECK(error); });
			
			frigg::run(frigg::move(action), allocator.get());
		}else if(device->bars[k].type == PciDevice::kBarMemory) {
			managarm::hw::PciBar<Allocator> bar_response(*allocator);
			bar_response.set_io_type(managarm::hw::IoType::MEMORY);
			bar_response.set_address(device->bars[k].address);
			bar_response.set_length(device->bars[k].length);
			response.add_bars(frigg::move(bar_response));

			auto action = pipe.sendDescriptorResp(device->bars[k].handle, eventHub, 1, 1 + k)
			+ frigg::lift([=] (HelError error) { HEL_SOFT_CHECK(error); });
			
			frigg::run(frigg::move(action), allocator.get());
		}else{
			assert(device->bars[k].type == PciDevice::kBarNone);

			managarm::hw::PciBar<Allocator> bar_response(*allocator);
			bar_response.set_io_type(managarm::hw::IoType::NONE);
			response.add_bars(frigg::move(bar_response));
		}
	}

	auto action = frigg::compose([=] (frigg::String<Allocator> *serialized, 
			managarm::hw::PciDevice<Allocator> *response) {
		response->SerializeToString(serialized);
		
		return pipe.sendStringResp(serialized->data(), serialized->size(), eventHub, 1, 0)
		+ frigg::lift([=] (HelError error) { HEL_CHECK(error); })
		+ pipe.sendDescriptorResp(device->interrupt.getHandle(), eventHub, 1, 7)
		+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
	}, frigg::String<Allocator>(*allocator), frigg::move(response));

	frigg::run(frigg::move(action), allocator.get());
}

void requireObject(int64_t object_id, helx::Pipe pipe) {
	for(size_t i = 0; i < allDevices.size(); i++) {
		if(allDevices[i]->mbusId != object_id)
			continue;
		frigg::runClosure<DeviceClosure>(*allocator, frigg::move(pipe), allDevices[i]);
		return;
	}

	assert(!"Could not find object id");
}

// --------------------------------------------------------
// Discovery functionality
// --------------------------------------------------------

size_t computeBarLength(uint32_t mask) {
	static_assert(sizeof(int) == 4, "Need long builtins");
	
	assert(mask);
	size_t length_bits = __builtin_ctz(mask);
	size_t decoded_bits = 32 - __builtin_clz(mask);
//  TODO: This requires libgcc
//	assert(__builtin_popcount(mask) == decoded_bits - length_bits);

	return size_t(1) << length_bits;
}

void checkPciFunction(uint32_t bus, uint32_t slot, uint32_t function) {
	uint16_t vendor = readPciHalf(bus, slot, function, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	uint8_t header_type = readPciByte(bus, slot, function, kPciHeaderType);
	if((header_type & 0x7F) == 0) {
		frigg::infoLogger() << "    Function " << function << ": Device" << frigg::endLog;
	}else if((header_type & 0x7F) == 1) {
		uint8_t secondary = readPciByte(bus, slot, function, kPciBridgeSecondary);
		frigg::infoLogger() << "    Function " << function
				<< ": PCI-to-PCI bridge to bus " << secondary << frigg::endLog;
	}else{
		frigg::infoLogger() << "    Function " << function
				<< ": Unexpected PCI header type " << (header_type & 0x7F) << frigg::endLog;
	}

	uint16_t device_id = readPciHalf(bus, slot, function, kPciDevice);
	uint8_t revision = readPciByte(bus, slot, function, kPciRevision);
	frigg::infoLogger() << "        Vendor: 0x" << frigg::logHex(vendor)
			<< ", device ID: 0x" << frigg::logHex(device_id)
			<< ", revision: " << revision << frigg::endLog;
	
	uint8_t class_code = readPciByte(bus, slot, function, kPciClassCode);
	uint8_t sub_class = readPciByte(bus, slot, function, kPciSubClass);
	uint8_t interface = readPciByte(bus, slot, function, kPciInterface);
	frigg::infoLogger() << "        Class: " << class_code
			<< ", subclass: " << sub_class << ", interface: " << interface << frigg::endLog;
	
	if((header_type & 0x7F) == 0) {
		uint16_t subsystem_vendor = readPciHalf(bus, slot, function, kPciRegularSubsystemVendor);
		uint16_t subsystem_device = readPciHalf(bus, slot, function, kPciRegularSubsystemDevice);
		frigg::infoLogger() << "        Subsystem vendor: 0x" << frigg::logHex(subsystem_vendor)
				<< ", device: 0x" << frigg::logHex(subsystem_device) << frigg::endLog;

		if(readPciHalf(bus, slot, function, kPciStatus) & 0x10) {
			// NOTE: the bottom two bits of each capability offset must be masked
			uint8_t offset = readPciByte(bus, slot, function, kPciRegularCapabilities) & 0xFC;
			while(offset != 0) {
				uint8_t capability = readPciByte(bus, slot, function, offset);
				uint8_t successor = readPciByte(bus, slot, function, offset + 1);
				
				auto dump = frigg::infoLogger() << "        Capability 0x"
						<< frigg::logHex(capability) << frigg::endLog;
				
				if(capability == 0x09) {
					uint8_t size = readPciByte(bus, slot, function, offset + 2);

					auto dump = frigg::infoLogger() << "            Bytes: ";
					for(size_t i = 2; i < size; i++) {
						if(i > 2)
							dump << ", ";
						dump << frigg::logHex(readPciByte(bus, slot, function, offset + i));
					}
					dump << frigg::endLog;
				}

				offset = successor & 0xFC;
			}
		}


		auto device = frigg::construct<PciDevice>(*allocator, bus, slot, function,
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

				frigg::Vector<uintptr_t, Allocator> ports(*allocator);
				for(uintptr_t offset = 0; offset < length; offset++)
					ports.push(address + offset);

				device->bars[i].type = PciDevice::kBarIo;
				device->bars[i].address = address;
				device->bars[i].length = length;
				HEL_CHECK(helAccessIo(ports.data(), ports.size(), &device->bars[i].handle));

				frigg::infoLogger() << "        I/O space BAR #" << i
						<< " at 0x" << frigg::logHex(address)
						<< ", length: " << length << " ports" << frigg::endLog;
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
				frigg::infoLogger() << "        32-bit memory BAR #" << i
						<< " at 0x" << frigg::logHex(address)
						<< ", length: " << length << " bytes" << frigg::endLog;
			}else if(((bar >> 1) & 3) == 2) {
				assert(i < 5); // otherwise there is no next bar.
				frigg::infoLogger() << "        64-bit memory BAR ignored for now!" << frigg::endLog;
				i++;
			}else{
				assert(!"Unexpected BAR type");
			}
		}

		// determine the interrupt line
		uint8_t line_number = readPciByte(bus, slot, function, kPciRegularInterruptLine);
		frigg::infoLogger() << "        Interrupt line: " << line_number << frigg::endLog;
		device->interrupt = helx::Irq::access(line_number);

		managarm::mbus::CntRequest<Allocator> request(*allocator);
		request.set_req_type(managarm::mbus::CntReqType::REGISTER);
		
		frigg::String<Allocator> vendor_str(*allocator, "pci-vendor:0x");
		vendor_str += frigg::uintToString(*allocator, vendor, 16);
		managarm::mbus::Capability<Allocator> vendor_cap(*allocator);
		vendor_cap.set_name(frigg::move(vendor_str));
		request.add_caps(frigg::move(vendor_cap));
		
		frigg::String<Allocator> device_str(*allocator, "pci-device:0x");
		device_str += frigg::uintToString(*allocator, device_id, 16);
		managarm::mbus::Capability<Allocator> device_cap(*allocator);
		device_cap.set_name(frigg::move(device_str));
		request.add_caps(frigg::move(device_cap));
		
		frigg::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);
		HelError send_error;
		mbusPipe.sendStringSync(serialized.data(), serialized.size(),
				eventHub, 123, 0, kHelRequest, send_error);
		HEL_CHECK(send_error);

		uint8_t buffer[128];
		HelError error;
		size_t length;
		mbusPipe.recvStringRespSync(buffer, 128, eventHub, 123, 0, error, length);
		HEL_CHECK(error);
		
		managarm::mbus::SvrResponse<Allocator> response(*allocator);
		response.ParseFromArray(buffer, length);
		
		device->mbusId = response.object_id();
		frigg::infoLogger() << "        ObjectID " << response.object_id() << frigg::endLog;

		allDevices.push(device);
	}
}

void checkPciDevice(uint32_t bus, uint32_t slot) {
	uint16_t vendor = readPciHalf(bus, slot, 0, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	frigg::infoLogger() << "Bus: " << bus << ", slot " << slot << frigg::endLog;
	
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

