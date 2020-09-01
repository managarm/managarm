#pragma once
#include <stdint.h>

namespace common::x86 {

struct Tss64 {
	uint32_t reserved0;
	uint64_t rsp0;
	uint64_t rsp1;
	uint64_t rsp2;
	uint64_t reserved1;
	uint64_t ist1;
	uint64_t ist2;
	uint64_t ist3;
	uint64_t ist4;
	uint64_t ist5;
	uint64_t ist6;
	uint64_t ist7;
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t ioMapOffset;
	uint8_t ioBitmap[8192];
	uint8_t ioAllOnes;
} __attribute__ (( packed ));

inline void initializeTss64(Tss64 *tss) {
	tss->ioMapOffset = __builtin_offsetof(Tss64, ioBitmap);

	for(int i = 0; i < 8192; i++)
		tss->ioBitmap[i] = 0xFF;
	tss->ioAllOnes = 0xFF;
}

} // namespace common::x86
