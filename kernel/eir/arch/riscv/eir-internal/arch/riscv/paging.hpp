#pragma once

#include <stdint.h>

namespace eir {

constexpr uint32_t getPPN0FromAddress(address_t addr) {
	return (addr & 0x1FF000) >> 12;
}

constexpr uint32_t getPPN1FromAddress(address_t addr) {
	return (addr & 0x3FE00000) >> 21;
}

constexpr uint32_t getPPN2FromAddress(address_t addr) {
	return (addr & 0xFFFFFFC0000000) >> 29;
}

constexpr uint32_t getVPN2FromAddress(address_t addr) {
	return (addr & 0x7FC0000000) >> 29;
}

struct sv39_page_table_entry {
	bool valid : 1;
	bool read : 1;
	bool write : 1;
	bool execute : 1;
	bool user : 1;
	bool global : 1;
	bool accessed : 1;
	bool dirty : 1;

	uint8_t rsw : 2;		// Ignored by implementations.

	uint16_t ppn0 : 9;
	uint16_t ppn1 : 9;
	uint32_t ppn2 : 26;

	uint16_t : 10; 			// Reserved

	constexpr void packPhyiscalAddress(uintptr_t subtable) {
		ppn0 = getPPN0FromAddress(subtable);
		ppn1 = getPPN1FromAddress(subtable);
		ppn2 = getPPN2FromAddress(subtable);
	}

	sv39_page_table_entry* getSubtable() {
		uintptr_t addr = (ppn0 << 12) | (ppn1 << 21) | (ppn2 << 30);
		return (sv39_page_table_entry*)addr;
	}
} __attribute__((packed));

extern sv39_page_table_entry* pt2;

constexpr bool isLeaf(sv39_page_table_entry entry) {
	return !(entry.read && entry.write && entry.execute);
}

// Level is how many PPNs make it to the translated address.
constexpr uint64_t convertEntryToAddress(sv39_page_table_entry* entry, uint16_t page_offset, int level) {
	uint64_t mask = 0;
	switch(level) {
		case 0: mask = 0x3FFFFFF0000000; break;
		case 1: mask = 0x3FFFFFFFF80000; break;
		case 2: mask = 0x3FFFFFFFFFFC00; break;
	}
	uint64_t entry_int = *((uint64_t*)entry);
	return (entry_int & mask) << 2;
}

}