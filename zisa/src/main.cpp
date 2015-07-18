
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <hel.h>
#include <hel-syscalls.h>

uint8_t ioInByte(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t out_value asm("al");
	asm volatile ( "inb %%dx, %%al" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}
uint16_t ioInShort(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t out_value asm("ax");
	asm volatile ( "inw %%dx, %%ax" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}

void ioOutByte(uint16_t port, uint8_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t in_value asm("al") = value;
	asm volatile ( "outb %%al, %%dx" : : "r" (in_port), "r" (in_value) );
}

enum AtaPorts {
	kPortReadData = 0,
	kPortWriteSectorCount = 2,
	kPortWriteLba1 = 3,
	kPortWriteLba2 = 4,
	kPortWriteLba3 = 5,
	kPortWriteDevice = 6,
	kPortWriteCommand = 7,
	kPortReadStatus = 7,
};

enum AtaCommands {
	kCommandReadSectorsExt = 0x24
};

enum AtaFlags {
	kStatusDrq = 0x08,
	kStatusBsy = 0x80,

	kDeviceSlave = 0x10,
	kDeviceLba = 0x40
};

void readAta() {
	uintptr_t ports[] = { 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6 };

	HelHandle io_space;
	helAccessIo(ports, 9, &io_space);
	helEnableIo(io_space);

	uint16_t port_base = 0x1F0;
	
	uint16_t count = 1;
	uint64_t lba = 0;

	ioOutByte(port_base + kPortWriteDevice, kDeviceLba);
	
	ioOutByte(port_base + kPortWriteSectorCount, (count >> 8) & 0xFF);
	ioOutByte(port_base + kPortWriteLba1, (lba >> 24) & 0xFF);
	ioOutByte(port_base + kPortWriteLba2, (lba >> 32) & 0xFF);
	ioOutByte(port_base + kPortWriteLba3, (lba >> 40) & 0xFF);	
	
	ioOutByte(port_base + kPortWriteSectorCount, count & 0xFF);
	ioOutByte(port_base + kPortWriteLba1, lba & 0xFF);
	ioOutByte(port_base + kPortWriteLba2, (lba >> 8) & 0xFF);
	ioOutByte(port_base + kPortWriteLba3, (lba >> 16) & 0xFF);
	
	ioOutByte(port_base + kPortWriteCommand, kCommandReadSectorsExt);

	while(true) {
		uint8_t status = ioInByte(port_base + kPortReadStatus);
		if((status & kStatusBsy) != 0)
			continue;
		if((status & kStatusDrq) == 0)
			continue;
		break;
	}

	uint8_t buffer[512];
	for(int i = 0; i < 256; i++) {
		uint16_t word = ioInShort(port_base + kPortReadData);
		buffer[2 * i] = word & 0xFF;
		buffer[2 * i + 1] = (word >> 16) & 0xFF;
	}

	printf("%x\n", buffer[0]);
}

int main() {
	HelHandle event_handle;
	helCreateEventHub(&event_handle);

	HelHandle irq_handle;
	helAccessIrq(0, &irq_handle);
	helSubmitIrq(irq_handle, event_handle, 
		0, 0, 0);
	
	HelEvent list[8];
	size_t num_items;
	helWaitForEvents(event_handle, list, 8, 0, &num_items);

	printf("%d items\n", num_items);

	while(true) { }
}

