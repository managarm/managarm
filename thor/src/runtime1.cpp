
#include "kernel.hpp"

#include <frigg/arch_x86/machine.hpp>

namespace memory = frigg::memory;

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

void ThorRtThreadState::activate() {
	asm volatile ( "mov %0, %%gs:0x08" : : "r" (this) : "memory" );
}

void thorRtInitializeProcessor() {
	auto cpu_specific = memory::construct<ThorRtCpuSpecific>(*thor::kernelAlloc);
	
	// set up the kernel gs segment
	auto kernel_gs = memory::construct<ThorRtKernelGs>(*thor::kernelAlloc);
	kernel_gs->cpuSpecific = cpu_specific;
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexGsBase, (uintptr_t)kernel_gs);

	// setup a stack for syscalls
	size_t syscall_stack_size = 0x100000;
	void *syscall_stack_base = thor::kernelAlloc->allocate(syscall_stack_size);
	kernel_gs->syscallStackPtr = (void *)((uintptr_t)syscall_stack_base
			+ syscall_stack_size);

	// setup the gdt
	// note: the tss requires two slots in the gdt
	frigg::arch_x86::makeGdtNullSegment(cpu_specific->gdt, 0);
	frigg::arch_x86::makeGdtCode64SystemSegment(cpu_specific->gdt, 1);
	frigg::arch_x86::makeGdtCode64UserSegment(cpu_specific->gdt, 2);
	frigg::arch_x86::makeGdtFlatData32UserSegment(cpu_specific->gdt, 3);
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_specific->gdt, 4, nullptr, 0);

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 6 * 8;
	gdtr.pointer = cpu_specific->gdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	thorRtLoadCs(0x8);
	
	// setup the kernel tss
	frigg::arch_x86::initializeTss64(&cpu_specific->tssTemplate);
	cpu_specific->tssTemplate.ist1 = (uintptr_t)kernel_gs->syscallStackPtr;
	
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_specific->gdt, 4,
			&cpu_specific->tssTemplate, sizeof(frigg::arch_x86::Tss64));
	asm volatile ( "ltr %w0" : : "r" ( 0x20 ) );
	
	// setup the idt
	for(int i = 0; i < 256; i++)
		frigg::arch_x86::makeIdt64NullGate(cpu_specific->idt, i);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 0,
			0x8, (void *)&thorRtIsrDivideByZeroError, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 6,
			0x8, (void *)&thorRtIsrInvalidOpcode, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 8,
			0x8, (void *)&thorRtIsrDoubleFault, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 13,
			0x8, (void *)&thorRtIsrGeneralProtectionFault, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 14,
			0x8, (void *)&thorRtIsrPageFault, 1);

	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 64,
			0x8, (void *)&thorRtIsrIrq0, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 65,
			0x8, (void *)&thorRtIsrIrq1, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 66,
			0x8, (void *)&thorRtIsrIrq2, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 67,
			0x8, (void *)&thorRtIsrIrq3, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 68,
			0x8, (void *)&thorRtIsrIrq4, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 69,
			0x8, (void *)&thorRtIsrIrq5, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 70,
			0x8, (void *)&thorRtIsrIrq6, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 71,
			0x8, (void *)&thorRtIsrIrq7, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 72,
			0x8, (void *)&thorRtIsrIrq8, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 73,
			0x8, (void *)&thorRtIsrIrq9, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 74,
			0x8, (void *)&thorRtIsrIrq10, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 75,
			0x8, (void *)&thorRtIsrIrq11, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 76,
			0x8, (void *)&thorRtIsrIrq12, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 77,
			0x8, (void *)&thorRtIsrIrq13, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 78,
			0x8, (void *)&thorRtIsrIrq14, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(cpu_specific->idt, 79,
			0x8, (void *)&thorRtIsrIrq15, 1);
	
	frigg::arch_x86::makeIdt64IntUserGate(cpu_specific->idt, 0x80,
			0x8, (void *)&thorRtIsrSyscall, 1);

	frigg::arch_x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = cpu_specific->idt;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) );
}

void thorRtEnableTss(frigg::arch_x86::Tss64 *tss_pointer) {
	ThorRtCpuSpecific *cpu_specific;
	asm volatile ( "mov %%gs:0x18, %0" : "=r" (cpu_specific) );

	tss_pointer->ist1 = cpu_specific->tssTemplate.ist1;
	
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_specific->gdt, 4,
			tss_pointer, sizeof(frigg::arch_x86::Tss64));
	asm volatile ( "ltr %w0" : : "r" ( 0x20 ) );
}

ThorRtKernelGs::ThorRtKernelGs()
: cpuContext(nullptr), threadState(nullptr), syscallStackPtr(nullptr),
		cpuSpecific(nullptr) { }

void thorRtSetCpuContext(void *context) {
	asm volatile ( "mov %0, %%gs:0" : : "r" (context) : "memory" );
}

void *thorRtGetCpuContext() {
	void *context;
	asm volatile ( "mov %%gs:0, %0" : "=r" (context) );
	return context;
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

