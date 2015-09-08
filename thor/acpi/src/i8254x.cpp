
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <hel.h>
#include <hel-syscalls.h>

#include <frigg/atomic.hpp>

#include "pci.hpp"

enum {
	kRegEerd = 0x14,
	kRegIms = 0xD0,
};

enum : uint32_t {
	kEerdStart = 1,
	kEerdDone = 16,
	kEerdAddressShift = 8,
	kEerdDataShift = 16,
};

void *baseAddress;

template<typename T>
T peekRegister(int offset) {
	return frigg::volatileRead((T *)((uintptr_t)baseAddress + offset));
}

template<typename T>
void pokeRegister(int offset, T value) {
	frigg::volatileWrite((T *)((uintptr_t)baseAddress + offset), value);
}

uint16_t peekEeprom(uint32_t address) {
	pokeRegister<uint32_t>(kRegEerd, kEerdStart | (address << kEerdAddressShift));
	
	uint32_t eerd;
	do {
		eerd = peekRegister<uint32_t>(kRegEerd);
	} while((eerd & kEerdDone) == 0);

	return eerd >> kEerdDataShift;
}

void initializeDevice(PciDevice *device) {
	printf("Initialize\n");

	HEL_CHECK(helMapMemory(device->bars[0].handle, kHelNullHandle, nullptr,
			device->bars[0].length, kHelMapReadWrite, &baseAddress));
	
	uint16_t mac1 = peekEeprom(0);
	uint16_t mac2 = peekEeprom(1);
	uint16_t mac3 = peekEeprom(2);
	uint8_t mac[] = { mac1 & 0xFF, mac1 >> 8, mac2 & 0xFF, mac2 >> 8, mac3 & 0xFF, mac3 >> 8 };
	printf("MAC address: %X:%X:%X:%X:%X:%X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	pokeRegister(kRegIms, 0x1F6DC);
}

