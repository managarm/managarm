
#include "../../include/types.hpp"
#include "../../include/arch_x86/idt.hpp"

namespace frigg {
namespace arch_x86 {

void makeIdt64NullGate(uint32_t *idt, int entry) {
	idt[entry * 4 + 0] = 0;
	idt[entry * 4 + 1] = kIdtWord1InterruptGate;
	idt[entry * 4 + 2] = 0;
	idt[entry * 4 + 3] = 0;
}

void makeIdt64IntGate(uint32_t *idt, int entry, int segment, void *handler) {
	uintptr_t offset = (uintptr_t)handler;
	idt[entry * 4 + 0] = ((uint32_t)offset & 0xFFFF) | ((uint32_t)segment << 16);
	idt[entry * 4 + 1] = kIdtWord1InterruptGate | kIdtWord1Present
			| ((uint32_t)offset & 0xFFFF0000);
	idt[entry * 4 + 2] = (uint32_t)(offset >> 32);
	idt[entry * 4 + 3] = 0;
}

}} // namespace frigg::arch_x86

