#pragma once

#include <stdint.h>

namespace common::x86 {

enum IdtFlags : uint32_t {
	kIdtWord1InterruptGate = 0x0E00,
	kIdtWord1User = 0x6000,
	kIdtWord1Present = 0x8000
};

struct Idtr {
	uint16_t limit;
	uint32_t *pointer;
} __attribute__ (( packed ));

inline void makeIdt64NullGate(uint32_t *idt, int entry) {
	idt[entry * 4 + 0] = 0;
	idt[entry * 4 + 1] = kIdtWord1InterruptGate;
	idt[entry * 4 + 2] = 0;
	idt[entry * 4 + 3] = 0;
}

inline void makeIdt64IntSystemGate(uint32_t *idt, int entry,
		int segment, void *handler, int ist) {
	uintptr_t offset = (uintptr_t)handler;
	idt[entry * 4 + 0] = ((uint32_t)offset & 0xFFFF) | ((uint32_t)segment << 16);
	idt[entry * 4 + 1] = kIdtWord1InterruptGate | kIdtWord1Present
			| ((uint32_t)offset & 0xFFFF0000) | (uint32_t)ist;
	idt[entry * 4 + 2] = (uint32_t)(offset >> 32);
	idt[entry * 4 + 3] = 0;
}

inline void makeIdt64IntUserGate(uint32_t *idt, int entry,
		int segment, void *handler, int ist) {
	uintptr_t offset = (uintptr_t)handler;
	idt[entry * 4 + 0] = ((uint32_t)offset & 0xFFFF) | ((uint32_t)segment << 16);
	idt[entry * 4 + 1] = kIdtWord1InterruptGate | kIdtWord1Present | kIdtWord1User
			| ((uint32_t)offset & 0xFFFF0000) | (uint32_t)ist;
	idt[entry * 4 + 2] = (uint32_t)(offset >> 32);
	idt[entry * 4 + 3] = 0;
}

} // namespace common::x86
