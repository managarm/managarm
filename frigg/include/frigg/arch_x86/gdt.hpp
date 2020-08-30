#ifndef FRIGG_ARCH_X86_GDT_HPP
#define FRIGG_ARCH_X86_GDT_HPP

#include <stdint.h>

namespace frigg {
namespace arch_x86 {

enum GdtFlags : uint32_t {
	kGdtWord1CodeSegment = 0x1800,
	kGdtWord1DataSegment = 0x1200,
	kGdtWord1TssDescriptor = 0x0900,
	kGdtWord1User = 0x6000,
	kGdtWord1Present = 0x8000,
	kGdtWord1Long = 0x00200000,
	kGdtWord1Default = 0x00400000,
	kGdtWord1Granularity = 0x00800000
};

struct Gdtr {
	uint16_t limit;
	uint32_t *pointer;
} __attribute__ (( packed ));

void makeGdtNullSegment(uint32_t *gdt, int entry);
void makeGdtFlatCode32SystemSegment(uint32_t *gdt, int entry);
void makeGdtFlatData32SystemSegment(uint32_t *gdt, int entry);
void makeGdtFlatData32UserSegment(uint32_t *gdt, int entry);
void makeGdtCode64SystemSegment(uint32_t *gdt, int entry);
void makeGdtCode64UserSegment(uint32_t *gdt, int entry);
void makeGdtTss64Descriptor(uint32_t *gdt, int entry, void *tss,
		size_t size);

}} // namespace frigg::arch_x86

#endif
