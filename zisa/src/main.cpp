
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <hel.h>
#include <hel-syscalls.h>

uint8_t ioInByte(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t out_value asm("al");
	asm volatile ( "inb %%dx, %%al" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}

void ioOutByte(uint16_t port, uint8_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t in_value asm("al") = value;
	asm volatile ( "outb %%al, %%dx" : : "r" (in_port), "r" (in_value) );
}

int main() {
	uintptr_t ports[] = { 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6 };

	HelHandle io_space;
	helAccessIo(ports, 9, &io_space);
	helEnableIo(io_space);

	uint8_t status = ioInByte(0x3F6);
	
	printf("Status port: %x\n", status);
}

