
#include "kernel.hpp"

#include <frigg/arch_x86/machine.hpp>

namespace memory = frigg::memory;
namespace debug = frigg::debug;

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

ThorRtThreadState::ThorRtThreadState() {
	memset(&threadTss, 0, sizeof(frigg::arch_x86::Tss64));
	frigg::arch_x86::initializeTss64(&threadTss);
}

void ThorRtThreadState::activate() {
	// set the current general state pointer
	asm volatile ( "mov %0, %%gs:0x08" : : "r" (&generalState) : "memory" );
	
	// setup the thread's tss segment
	ThorRtCpuSpecific *cpu_specific;
	asm volatile ( "mov %%gs:0x18, %0" : "=r" (cpu_specific) );
	threadTss.ist1 = cpu_specific->tssTemplate.ist1;
	
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_specific->gdt, 4,
			&threadTss, sizeof(frigg::arch_x86::Tss64));
	asm volatile ( "ltr %w0" : : "r" ( 0x20 ) );
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

struct ThorRtTrampolineData {
	uint32_t status;
};

template<typename T>
void writeVolatile(T *pointer, T value) {
	*const_cast<volatile T *>(pointer) = value;
}
template<typename T>
T readVolatile(T *pointer) {
	return *const_cast<volatile T *>(pointer);
}

extern uint8_t trampolineStart[];

enum {
	kIcrDeliverInit = 0x500,
	kIcrDeliverStartup = 0x600,
	kIcrLevelAssert = 0x4000,
	kIcrTriggerLevel = 0x8000,
};

void thorRtBootSecondary() {
	uint64_t apic_info = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrLocalApicBase);
	ASSERT((apic_info & (1 << 8)) != 0); // this processor is the BSP
	ASSERT((apic_info & (1 << 11)) != 0); // local APIC is enabled
	uint64_t apic_base = apic_info & 0xFFFFF000;
	thor::infoLogger->log() << "Local APIC at " << (void *)apic_base << debug::Finish();

	auto apic_spurious = thor::accessPhysical<uint32_t>(apic_base + 0x00F0);
	auto apic_icr_low = thor::accessPhysical<uint32_t>(apic_base + 0x0300);
	auto apic_icr_high = thor::accessPhysical<uint32_t>(apic_base + 0x0310);
	auto apic_lvt_timer = thor::accessPhysical<uint32_t>(apic_base + 0x0320);
	auto apic_initial_count = thor::accessPhysical<uint32_t>(apic_base + 0x0380);
	
	// enable the local apic
	uint32_t spurious_vector = 0x81;
	writeVolatile<uint32_t>(apic_spurious, spurious_vector | 0x100);
	
	// copy the trampoline code into low physical memory
	memcpy(thor::physicalToVirtual(0x10000), trampolineStart, 0x1000);

	// setup the trampoline data area
	auto data = thor::accessPhysical<ThorRtTrampolineData>(0x11000);
	writeVolatile<uint32_t>(&data->status, 0);

	asm volatile ( "" : : : "memory" );

	uint32_t secondary_apic_id = 1;
	
	// send the init ipi
	writeVolatile<uint32_t>(apic_icr_high, secondary_apic_id << 24);
	writeVolatile<uint32_t>(apic_icr_low, kIcrDeliverInit
			| kIcrTriggerLevel | kIcrLevelAssert);
	
	// send the init ipi de-assert
	writeVolatile<uint32_t>(apic_icr_high, secondary_apic_id << 24);
	writeVolatile<uint32_t>(apic_icr_low, kIcrDeliverInit
			| kIcrTriggerLevel);

	// send the startup ipi
	uint32_t vector = 0x10; // determines the startup code page
	writeVolatile<uint32_t>(apic_icr_high, secondary_apic_id << 24);
	writeVolatile<uint32_t>(apic_icr_low, vector | kIcrDeliverStartup);

	asm volatile ( "" : : : "memory" );

	thor::infoLogger->log() << "Waiting for AP to start" << debug::Finish();
	while(readVolatile<uint32_t>(&data->status) == 0) {
		// do nothing
	}
	thor::infoLogger->log() << "AP is running" << debug::Finish();
}

ThorRtKernelGs::ThorRtKernelGs()
: cpuContext(nullptr), generalState(nullptr), syscallStackPtr(nullptr),
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

namespace thor {

void BochsSink::print(char c) {
	frigg::arch_x86::ioOutByte(0xE9, c);
}
void BochsSink::print(const char *str) {
	while(*str != 0)
		frigg::arch_x86::ioOutByte(0xE9, *str++);
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
	uint8_t a1 = frigg::arch_x86::ioInByte(kPic1Data);
	uint8_t a2 = frigg::arch_x86::ioInByte(kPic2Data);

	// start initilization
	frigg::arch_x86::ioOutByte(kPic1Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic1Data, offset);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, offset + 8);
	ioWait();

	// setup cascade
	frigg::arch_x86::ioOutByte(kPic1Data, 4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, 2);
	ioWait();

	frigg::arch_x86::ioOutByte(kPic1Data, kIcw4Mode8086);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, kIcw4Mode8086);
	ioWait();

	// restore saved masks
	frigg::arch_x86::ioOutByte(kPic1Data, a1);
	frigg::arch_x86::ioOutByte(kPic2Data, a2);
}

void thorRtSetupIrqs() {
	thorRtRemapPic(64);
}

void thorRtAcknowledgeIrq(int irq) {
	if(irq >= 8)
		frigg::arch_x86::ioOutByte(kPic2Command, kPicEoi);
	frigg::arch_x86::ioOutByte(kPic1Command, kPicEoi);
}

