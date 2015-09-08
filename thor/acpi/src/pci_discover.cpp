
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <vector>

#include <hel.h>
#include <hel-syscalls.h>

#include "pci.hpp"

// TODO: move i8254 driver to own file
void initializeDevice(PciDevice *device);

void checkPciFunction(uint32_t bus, uint32_t slot, uint32_t function) {
	uint16_t vendor = readPciHalf(bus, slot, function, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	uint8_t header_type = readPciByte(bus, slot, function, kPciHeaderType);
	if((header_type & 0x7F) == 0) {
		printf("    Function %d: Device\n", function);
	}else if((header_type & 0x7F) == 1) {
		uint8_t secondary = readPciByte(bus, slot, function, kPciSecondaryBus);
		printf("    Function %d: PCI-to-PCI bridge to bus %d\n", function, secondary);
	}else{
		printf("    Function %d: Unexpected PCI header type %d\n", function, header_type & 0x7F);
	}

	uint16_t device_id = readPciHalf(bus, slot, function, kPciDevice);
	uint8_t revision = readPciByte(bus, slot, function, kPciRevision);
	printf("        Vendor: 0x%X, device ID: 0x%X, revision: 0x%X\n", vendor, device_id, revision);
	
	uint8_t class_code = readPciByte(bus, slot, function, kPciClassCode);
	uint8_t sub_class = readPciByte(bus, slot, function, kPciSubClass);
	uint8_t interface = readPciByte(bus, slot, function, kPciInterface);
	printf("        Class: 0x%X, subclass: 0x%X, interface: 0x%X\n",
			class_code, sub_class, interface);
	
	if((header_type & 0x7F) == 0) {
		PciDevice device(bus, slot, function, vendor, device_id, revision,
				class_code, sub_class, interface);
		
		// determine the BARs
		for(int i = 0; i < 6; i++) {
			uint32_t offset = kPciBar0 + i * 4;
			uint32_t bar = readPciWord(bus, slot, function, offset);
			if(bar == 0)
				continue;
			
			if((bar & 1) != 0) {
				uint32_t address = bar & 0xFFFFFFFC;
				
				// write all 1s to the BAR and read it back to determine this its length
				writePciWord(bus, slot, function, offset, 0xFFFFFFFC);
				uint32_t mask = readPciWord(bus, slot, function, offset);
				writePciWord(bus, slot, function, offset, bar);
				uint32_t length = ~(mask & 0xFFFFFFFC) + 1;

				std::vector<uintptr_t> ports;
				for(uintptr_t offset = 0; offset < length; offset++)
					ports.push_back(address + offset);

				device.bars[i].type = PciDevice::kBarIo;
				device.bars[i].length = length;
				HEL_CHECK(helAccessIo(ports.data(), ports.size(), &device.bars[i].handle));

				printf("        I/O space BAR #%d at 0x%X, length: %u ports\n",
						i, address, length);
			}else if(((bar >> 1) & 3) == 0) {
				uint32_t address = bar & 0xFFFFFFF0;
				
				// write all 1s to the BAR and read it back to determine this its length
				writePciWord(bus, slot, function, offset, 0xFFFFFFF0);
				uint32_t mask = readPciWord(bus, slot, function, offset);
				writePciWord(bus, slot, function, offset, bar);
				uint32_t length = ~(mask & 0xFFFFFFF0) + 1;
				
				device.bars[i].type = PciDevice::kBarMemory;
				device.bars[i].length = length;
				HEL_CHECK(helAccessPhysical(address, length, &device.bars[i].handle));

				printf("        32-bit memory BAR #%d at 0x%X, length: %u bytes\n",
						i, address, length);
			}else if(((bar >> 1) & 3) == 2) {
				assert(!"Handle 64-bit memory BARs");
			}else{
				assert(!"Unexpected BAR type");
			}
		}

		if(vendor == 0x8086 && device_id == 0x100E)
			initializeDevice(&device);
	}
}

void checkPciDevice(uint32_t bus, uint32_t slot) {
	uint16_t vendor = readPciHalf(bus, slot, 0, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	printf("Bus: %d, slot %d\n", bus, slot);
	
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

