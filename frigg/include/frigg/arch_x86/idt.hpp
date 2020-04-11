#ifndef FRIGG_ARCH_X86_IDT_HPP
#define FRIGG_ARCH_X86_IDT_HPP

#include <frigg/macros.hpp>

namespace frigg FRIGG_VISIBILITY {
namespace arch_x86 {

enum IdtFlags : uint32_t {
	kIdtWord1InterruptGate = 0x0E00,
	kIdtWord1User = 0x6000,
	kIdtWord1Present = 0x8000
};

struct Idtr {
	uint16_t limit;
	uint32_t *pointer;
} __attribute__ (( packed ));

void makeIdt64NullGate(uint32_t *idt, int entry);
void makeIdt64IntSystemGate(uint32_t *idt, int entry,
		int segment, void *handler, int ist);
void makeIdt64IntUserGate(uint32_t *idt, int entry,
		int segment, void *handler, int ist);

}} // namespace frigg::arch_x86

#endif