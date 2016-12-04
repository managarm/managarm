
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <hel.h>

#include "pci.hpp"

uint32_t readPciWord(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset) {
	assert(bus < 256 && slot < 32 && function < 8);
	assert((offset % 4) == 0 && offset < 256);
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) | offset | 0x80000000;
	asm volatile ( "outl %0, %1" : : "a" (address), "d" (uint16_t(0xCF8)) );

	uint32_t result;
	asm volatile ( "inl %1, %0" : "=a" (result) : "d" (uint16_t(0xCFC)) );
	return result;
}

uint16_t readPciHalf(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset) {
	assert((offset % 2) == 0);
	uint32_t word = readPciWord(bus, slot, function, offset & ~uint32_t(3));
	return (word >> ((offset & 3)) * 8) & 0xFFFF;
}

uint8_t readPciByte(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset) {
	uint32_t word = readPciWord(bus, slot, function, offset & ~uint32_t(3));
	return (word >> ((offset & 3)) * 8) & 0xFF;
}

void writePciWord(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset, uint32_t value) {
	assert(bus < 256 && slot < 32 && function < 8);
	assert((offset % 4) == 0 && offset < 256);
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) | offset | 0x80000000;
	asm volatile ( "outl %0, %1" : : "a" (address), "d" (uint16_t(0xCF8)) );
	asm volatile ( "outl %0, %1" : : "a" (value), "d" (uint16_t(0xCFC)) );
}

