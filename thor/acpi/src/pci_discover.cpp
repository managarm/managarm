
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <hel.h>
#include <hel-syscalls.h>

enum {
	// general PCI header fields
	kPciVendor = 0,
	kPciDevice = 2,
	kPciSubClass = 0x0A,
	kPciClassCode = 0x0B,
	kPciHeaderType = 0x0E,

	// PCI-to-PCI bridge header fields
	kPciSecondaryBus = 0x19
};

uint32_t pciReadWord(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset) {
	assert(bus < 256);
	assert(slot < 32);
	assert(function < 8);
	assert((offset % 4) == 0 && offset < 256);
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) | offset | 0x80000000;
	asm volatile ( "outl %0, %1" : : "a" (address), "d" (uint16_t(0xCF8)) );

	uint32_t result;
	asm volatile ( "inl %1, %0" : "=a" (result) : "d" (uint16_t(0xCFC)) );
	return result;
}

uint16_t pciReadHalf(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset) {
	assert((offset % 2) == 0);
	uint32_t word = pciReadWord(bus, slot, function, offset & ~uint32_t(3));
	return (word >> ((offset & 3)) * 8) & 0xFFFF;
}

uint8_t pciReadByte(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset) {
	uint32_t word = pciReadWord(bus, slot, function, offset & ~uint32_t(3));
	return (word >> ((offset & 3)) * 8) & 0xFF;
}

void pciCheckFunction(uint32_t bus, uint32_t slot, uint32_t function) {
	uint16_t vendor = pciReadHalf(bus, slot, function, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	uint8_t header_type = pciReadByte(bus, slot, function, kPciHeaderType);
	if((header_type & 0x7F) == 0) {
		printf("    Function %d: Device\n", function);
	}else if((header_type & 0x7F) == 1) {
		uint8_t secondary = pciReadByte(bus, slot, function, kPciSecondaryBus);
		printf("    Function %d: PCI-to-PCI bridge to bus %d\n", function, secondary);
	}else{
		printf("    Function %d: Unexpected PCI header type %d\n", function, header_type & 0x7F);
	}

	uint16_t device_id = pciReadHalf(bus, slot, function, kPciDevice);
	uint8_t class_code = pciReadByte(bus, slot, function, kPciClassCode);
	uint8_t sub_class = pciReadByte(bus, slot, function, kPciSubClass);
	printf("        Vendor: 0x%X, device ID: 0x%X"
			", class: 0x%X, subclass: 0x%X\n", vendor, device_id, class_code, sub_class);
}

void pciCheckDevice(uint32_t bus, uint32_t slot) {
	uint16_t vendor = pciReadHalf(bus, slot, 0, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	printf("Bus: %d, slot %d\n", bus, slot);
	
	uint8_t header_type = pciReadByte(bus, slot, 0, kPciHeaderType);
	if((header_type & 0x80) != 0) {
		for(uint32_t function = 0; function < 8; function++)
			pciCheckFunction(bus, slot, function);
	}else{
		pciCheckFunction(bus, slot, 0);
	}
}

void pciCheckBus(uint32_t bus) {
	for(uint32_t slot = 0; slot < 32; slot++)
		pciCheckDevice(bus, slot);
}

void pciDiscover() {
	uintptr_t ports[] = { 0xCF8, 0xCF9, 0xCFA, 0xCFB, 0xCFC, 0xCFD, 0xCFE, 0xCFF };
	HelHandle io_handle;
	HEL_CHECK(helAccessIo(ports, 8, &io_handle));
	HEL_CHECK(helEnableIo(io_handle));

//	for(uint32_t bus = 0; bus < 256; bus++)
	pciCheckBus(0);
}

