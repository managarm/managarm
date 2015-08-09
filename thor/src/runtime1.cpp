
#include "kernel.hpp"

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
extern "C" void thorRtIsrSyscall();

ThorRtThreadState *thorRtUserContext = nullptr;

uint32_t *thorRtGdtPointer;

void thorRtInvalidateSpace() {
	asm volatile ("movq %%cr3, %%rax\n\t"
		"movq %%rax, %%cr3" : : : "%rax");
}

void thorRtEnableInts() {
	asm volatile ( "sti" );
}
void thorRtDisableInts() {
	asm volatile ( "cli" );
}

void thorRtInitializeProcessor() {
	uintptr_t gdt_page = thor::physicalAllocator->allocate(1);
	thorRtGdtPointer = (uint32_t *)thor::physicalToVirtual(gdt_page);
	frigg::arch_x86::makeGdtNullSegment(thorRtGdtPointer, 0);
	frigg::arch_x86::makeGdtCode64SystemSegment(thorRtGdtPointer, 1);
	frigg::arch_x86::makeGdtCode64UserSegment(thorRtGdtPointer, 2);
	frigg::arch_x86::makeGdtFlatData32UserSegment(thorRtGdtPointer, 3);
	frigg::arch_x86::makeGdtTss64Descriptor(thorRtGdtPointer, 4, nullptr, 0);

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 6 * 8;
	gdtr.pointer = thorRtGdtPointer;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	thorRtLoadCs(0x8);
	
	uintptr_t idt_page = thor::physicalAllocator->allocate(1);
	uint32_t *idt_pointer = (uint32_t *)thor::physicalToVirtual(idt_page);
	for(int i = 0; i < 256; i++)
		frigg::arch_x86::makeIdt64NullGate(idt_pointer, i);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 0,
			0x8, (void *)&thorRtIsrDivideByZeroError, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 6,
			0x8, (void *)&thorRtIsrInvalidOpcode, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 8,
			0x8, (void *)&thorRtIsrDoubleFault, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 13,
			0x8, (void *)&thorRtIsrGeneralProtectionFault, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 14, 0x8, (void *)&thorRtIsrPageFault, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 64, 0x8, (void *)&thorRtIsrIrq0, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 65, 0x8, (void *)&thorRtIsrIrq1, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 66, 0x8, (void *)&thorRtIsrIrq2, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 67, 0x8, (void *)&thorRtIsrIrq3, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 68, 0x8, (void *)&thorRtIsrIrq4, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 69, 0x8, (void *)&thorRtIsrIrq5, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 70, 0x8, (void *)&thorRtIsrIrq6, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 71, 0x8, (void *)&thorRtIsrIrq7, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 72, 0x8, (void *)&thorRtIsrIrq8, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 73, 0x8, (void *)&thorRtIsrIrq9, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 74, 0x8, (void *)&thorRtIsrIrq10, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 75, 0x8, (void *)&thorRtIsrIrq11, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 76, 0x8, (void *)&thorRtIsrIrq12, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 77, 0x8, (void *)&thorRtIsrIrq13, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 78, 0x8, (void *)&thorRtIsrIrq14, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(idt_pointer, 79, 0x8, (void *)&thorRtIsrIrq15, 1);
	frigg::arch_x86::makeIdt64IntUserGate(idt_pointer, 0x80, 0x8, (void *)&thorRtIsrSyscall, 1);

	frigg::arch_x86::Idtr idtr;
	idtr.limit = 16 * 256;
	idtr.pointer = idt_pointer;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) );
}

void thorRtEnableTss(frigg::arch_x86::Tss64 *tss_pointer) {
	frigg::arch_x86::makeGdtTss64Descriptor(thorRtGdtPointer, 4,
			tss_pointer, sizeof(frigg::arch_x86::Tss64));

	asm volatile ( "ltr %w0" : : "r" ( 0x20 ) );
}

void ioWait() { }

uint8_t ioInByte(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t out_value asm("al");
	asm volatile ( "inb %%dx, %%al" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}

void ioOutByte(uint16_t port, uint8_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t in_value asm("al") = value;
	asm volatile ( "outb %%al, %%dx" : : "r" (in_port), "r" (in_value) );
}

namespace thor {

void BochsSink::print(char c) {
	ioOutByte(0xE9, c);
}
void BochsSink::print(const char *str) {
	while(*str != 0)
		ioOutByte(0xE9, *str++);
}

} // namespace thor

enum PicRegisters : uint16_t {
	kPic1Command = 0x20,
	kPic1Data = 0x21,
	kPic2Command = 0xA0,
	kPic2Data = 0xA1
};

enum PicBytes : uint8_t {
	kIcw1Icw4 = 0x01,
	kIcw1Single = 0x02,
	kIcw1Interval4 = 0x04,
	kIcw1Level = 0x08,
	kIcw1Init = 0x10,

	kIcw4Mode8086 = 0x01,
	kIcw4Auto = 0x02,
	kIcw4BufSlave = 0x08,
	kIcw4BufMaster = 0x0C,
	kIcw4Sfnm = 0x10,

	kPicEoi = 0x20
};

void thorRtRemapPic(int offset) {
	// save masks
	uint8_t a1 = ioInByte(kPic1Data);
	uint8_t a2 = ioInByte(kPic2Data);

	// start initilization
	ioOutByte(kPic1Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	ioOutByte(kPic2Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	ioOutByte(kPic1Data, offset);
	ioWait();
	ioOutByte(kPic2Data, offset + 8);
	ioWait();

	// setup cascade
	ioOutByte(kPic1Data, 4);
	ioWait();
	ioOutByte(kPic2Data, 2);
	ioWait();

	ioOutByte(kPic1Data, kIcw4Mode8086);
	ioWait();
	ioOutByte(kPic2Data, kIcw4Mode8086);
	ioWait();

	// restore saved masks
	ioOutByte(kPic1Data, a1);
	ioOutByte(kPic2Data, a2);
}

void thorRtSetupIrqs() {
	thorRtRemapPic(64);
}

void thorRtAcknowledgeIrq(int irq) {
	if(irq >= 8)
		ioOutByte(kPic2Command, kPicEoi);
	ioOutByte(kPic1Command, kPicEoi);
}

