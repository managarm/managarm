#include <stdint.h>
#include <x86/gdt.hpp>
#include <x86/machine.hpp>

namespace arch = common::x86;

uint32_t gdtEntries[4 * 2];

namespace eir {

void initArchCpu() {
	arch::makeGdtNullSegment(gdtEntries, 0);
	arch::makeGdtFlatCode32SystemSegment(gdtEntries, 1);
	arch::makeGdtFlatData32SystemSegment(gdtEntries, 2);
	arch::makeGdtCode64SystemSegment(gdtEntries, 3);

	arch::Gdtr gdtr = {4 * 8 - 1, gdtEntries};

	asm volatile("lgdt %0;"
	             "ljmp %1, $1f;"
	             "1:;"
	             "mov %2, %%ds;"
	             "mov %2, %%es;"
	             "mov %2, %%ss;"
	             "mov %3, %%fs;"
	             "mov %3, %%gs;"
	             :
	             : "m"(gdtr), "i"(0x08), "r"(0x10), "r"(0x00)
	             : "memory");
}

} // namespace eir
