
#include "../kernel.hpp"

extern "C" void thorRtIsrDivideByZeroError();
extern "C" void thorRtIsrInvalidOpcode();
extern "C" void thorRtIsrDoubleFault();
extern "C" void thorRtIsrGeneralProtectionFault();
extern "C" void thorRtIsrPageFault();
extern "C" void thorRtIsrIrq0();
extern "C" void thorRtIsrIrq1();
extern "C" void thorRtIsrIrq2();
extern "C" void thorRtIsrIrq3();
extern "C" void thorRtIsrIrq4();
extern "C" void thorRtIsrIrq5();
extern "C" void thorRtIsrIrq6();
extern "C" void thorRtIsrIrq7();
extern "C" void thorRtIsrIrq8();
extern "C" void thorRtIsrIrq9();
extern "C" void thorRtIsrIrq10();
extern "C" void thorRtIsrIrq11();
extern "C" void thorRtIsrIrq12();
extern "C" void thorRtIsrIrq13();
extern "C" void thorRtIsrIrq14();
extern "C" void thorRtIsrIrq15();

namespace thor {

void setupIdt(uint32_t *table) {
	frigg::arch_x86::makeIdt64IntSystemGate(table, 0,
			0x8, (void *)&thorRtIsrDivideByZeroError, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 6,
			0x8, (void *)&thorRtIsrInvalidOpcode, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 8,
			0x8, (void *)&thorRtIsrDoubleFault, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 13,
			0x8, (void *)&thorRtIsrGeneralProtectionFault, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 14,
			0x8, (void *)&thorRtIsrPageFault, 1);

	frigg::arch_x86::makeIdt64IntSystemGate(table, 64,
			0x8, (void *)&thorRtIsrIrq0, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 65,
			0x8, (void *)&thorRtIsrIrq1, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 66,
			0x8, (void *)&thorRtIsrIrq2, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 67,
			0x8, (void *)&thorRtIsrIrq3, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 68,
			0x8, (void *)&thorRtIsrIrq4, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 69,
			0x8, (void *)&thorRtIsrIrq5, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 70,
			0x8, (void *)&thorRtIsrIrq6, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 71,
			0x8, (void *)&thorRtIsrIrq7, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 72,
			0x8, (void *)&thorRtIsrIrq8, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 73,
			0x8, (void *)&thorRtIsrIrq9, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 74,
			0x8, (void *)&thorRtIsrIrq10, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 75,
			0x8, (void *)&thorRtIsrIrq11, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 76,
			0x8, (void *)&thorRtIsrIrq12, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 77,
			0x8, (void *)&thorRtIsrIrq13, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 78,
			0x8, (void *)&thorRtIsrIrq14, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 79,
			0x8, (void *)&thorRtIsrIrq15, 1);
}

void thorRtEnableInts() {
	asm volatile ( "sti" );
}

void thorRtDisableInts() {
	asm volatile ( "cli" );
}

} // namespace thor

