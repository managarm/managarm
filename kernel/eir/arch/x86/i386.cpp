#include <stdint.h>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/arch_x86/gdt.hpp>

namespace arch = frigg::arch_x86;

// TODO: eirRtLoadGdt could be written using inline assembly.
extern "C" void eirRtLoadGdt(uint32_t *pointer, uint32_t size);

uint32_t gdtEntries[4 * 2];

void initArchCpu() {
	arch::makeGdtNullSegment(gdtEntries, 0);
	arch::makeGdtFlatCode32SystemSegment(gdtEntries, 1);
	arch::makeGdtFlatData32SystemSegment(gdtEntries, 2);
	arch::makeGdtCode64SystemSegment(gdtEntries, 3);
	
	eirRtLoadGdt(gdtEntries, 4 * 8 - 1); 
}

