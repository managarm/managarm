#pragma once

#include <stdint.h>

namespace common::x86 {

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
} __attribute__((packed));

namespace detail {
inline void
makeGdtSegment(uint32_t *gdt, int entry, uint32_t offset, uint32_t limit, uint32_t word1_flags) {
	gdt[entry * 2 + 0] = (limit & 0xFFFF) | (offset << 16);
	gdt[entry * 2 + 1] =
	    ((offset >> 16) & 0xFF) | word1_flags | (limit & 0x000F0000) | (offset & 0xFF000000);
}
} // namespace detail

inline void makeGdtNullSegment(uint32_t *gdt, int entry) {
	gdt[entry * 2 + 0] = 0;
	gdt[entry * 2 + 1] = 0;
}

inline void makeGdtFlatCode32SystemSegment(uint32_t *gdt, int entry) {
	detail::makeGdtSegment(
	    gdt,
	    entry,
	    0,
	    0x000FFFFF,
	    kGdtWord1CodeSegment | kGdtWord1Present | kGdtWord1Default | kGdtWord1Granularity
	);
}

inline void makeGdtFlatData32SystemSegment(uint32_t *gdt, int entry) {
	detail::makeGdtSegment(
	    gdt,
	    entry,
	    0,
	    0x000FFFFF,
	    kGdtWord1DataSegment | kGdtWord1Present | kGdtWord1Default | kGdtWord1Granularity
	);
}
inline void makeGdtFlatData32UserSegment(uint32_t *gdt, int entry) {
	detail::makeGdtSegment(
	    gdt,
	    entry,
	    0,
	    0x000FFFFF,
	    kGdtWord1DataSegment | kGdtWord1User | kGdtWord1Present | kGdtWord1Default |
	        kGdtWord1Granularity
	);
}

inline void makeGdtCode64SystemSegment(uint32_t *gdt, int entry) {
	gdt[entry * 2 + 0] = 0;
	gdt[entry * 2 + 1] =
	    kGdtWord1CodeSegment | kGdtWord1Present | kGdtWord1Long | kGdtWord1Granularity;
}

inline void makeGdtCode64UserSegment(uint32_t *gdt, int entry) {
	gdt[entry * 2 + 0] = 0;
	gdt[entry * 2 + 1] = kGdtWord1CodeSegment | kGdtWord1User | kGdtWord1Present | kGdtWord1Long |
	                     kGdtWord1Granularity;
}

inline void makeGdtTss64Descriptor(uint32_t *gdt, int entry, void *tss, size_t size) {
	uint64_t address = (uint64_t)tss;
	gdt[entry * 2 + 0] = ((uint32_t)size & 0xFFFF) | ((address & 0xFFFF) << 16);
	gdt[entry * 2 + 1] = ((address >> 16) & 0xFF) | kGdtWord1TssDescriptor | kGdtWord1Present |
	                     ((uint32_t)size & 0x000F0000) | (address & 0xFF000000);
	gdt[entry * 2 + 2] = address >> 32;
	gdt[entry * 2 + 3] = 0;
}

} // namespace common::x86
