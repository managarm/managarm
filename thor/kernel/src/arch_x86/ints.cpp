
#include "../kernel.hpp"

extern char stubsPtr[], stubsLimit[];

extern "C" void earlyStubDivideByZero();
extern "C" void earlyStubOpcode();
extern "C" void earlyStubDouble();
extern "C" void earlyStubProtection();
extern "C" void earlyStubPage();

extern "C" void faultStubDivideByZero();
extern "C" void faultStubDebug();
extern "C" void faultStubBreakpoint();
extern "C" void faultStubOpcode();
extern "C" void faultStubNoFpu();
extern "C" void faultStubDouble();
extern "C" void faultStubProtection();
extern "C" void faultStubPage();

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

extern "C" void thorRtIsrPreempted();


namespace thor {

uint32_t earlyGdt[3 * 2];
uint32_t earlyIdt[256 * 4];

extern "C" void handleEarlyDivideByZeroFault(void *rip) {
	frigg::panicLogger.log() << "Division by zero during boot\n"
			<< "Faulting IP: " << rip << frigg::EndLog();
}

extern "C" void handleEarlyOpcodeFault(void *rip) {
	frigg::panicLogger.log() << "Invalid opcode during boot\n"
			<< "Faulting IP: " << rip << frigg::EndLog();
}

extern "C" void handleEarlyDoubleFault(uint64_t errcode, void *rip) {
	frigg::panicLogger.log() << "Double fault during boot\n"
			<< "Faulting IP: " << rip << frigg::EndLog();
}

extern "C" void handleEarlyProtectionFault(uint64_t errcode, void *rip) {
	frigg::panicLogger.log() << "Protection fault during boot\n"
			<< "Segment: " << errcode << "\n"
			<< "Faulting IP: " << rip << frigg::EndLog();
}

extern "C" void handleEarlyPageFault(uint64_t errcode, void *rip) {
	frigg::panicLogger.log() << "Page fault during boot\n"
			<< "Faulting IP: " << rip << frigg::EndLog();
}

void initializeProcessorEarly() {
	// setup the gdt
	frigg::arch_x86::makeGdtNullSegment(earlyGdt, 0);
	// for simplicity, match the layout with the "real" gdt we load later
	frigg::arch_x86::makeGdtCode64SystemSegment(earlyGdt, 1);
	frigg::arch_x86::makeGdtFlatData32SystemSegment(earlyGdt, 2);

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 3 * 8;
	gdtr.pointer = earlyGdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq $0x8\n"
			"\rpushq $.L_reloadEarlyCs\n"
			"\rlretq\n"
			".L_reloadEarlyCs:" );
	
	// setup the idt
	frigg::arch_x86::makeIdt64IntSystemGate(earlyIdt, 0, 0x8, (void *)&earlyStubDivideByZero, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(earlyIdt, 6, 0x8, (void *)&earlyStubOpcode, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(earlyIdt, 8, 0x8, (void *)&earlyStubDouble, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(earlyIdt, 13, 0x8, (void *)&earlyStubProtection, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(earlyIdt, 14, 0x8, (void *)&earlyStubPage, 0);
	
	frigg::arch_x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = earlyIdt;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) : "memory" );
}

void setupIdt(uint32_t *table) {
	frigg::arch_x86::makeIdt64IntSystemGate(table, 0,
			0x8, (void *)&faultStubDivideByZero, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 1, 0x8, (void *)&faultStubDebug, 0);
	frigg::arch_x86::makeIdt64IntUserGate(table, 3, 0x8, (void *)&faultStubBreakpoint, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 6,
			0x8, (void *)&faultStubOpcode, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 7, 0x8, (void *)&faultStubNoFpu, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 8,
			0x8, (void *)&faultStubDouble, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 13,
			0x8, (void *)&faultStubProtection, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 14,
			0x8, (void *)&faultStubPage, 0);

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

	//FIXME
//	frigg::arch_x86::makeIdt64IntSystemGate(table, 0x82,
//			0x8, (void *)&thorRtIsrPreempted, 0);
}

enum Domain {
	kDomNone,

	// the system is not running an executor, e.g.
	// we are processing an IRQ, NMI or MCE.
	kDomSystem,

	// the system is running an executor and we are in client code,
	// either in user-mode or in supervisor code.
	kDomClientUser,
	kDomClientSupervisor,

	// the system is running an executor in the kernel, e.g.
	// we are processing a system call or exception.
	kDomExecutorKernel
};

Domain determineDomain(uintptr_t cs) {
	switch(cs) {
	case 0x2B:
		return kDomClientUser;
	default:
		frigg::panicLogger.log() << "Unexpected CS segment" << frigg::EndLog();
		__builtin_unreachable();
	}
}

bool inStub(uintptr_t ip) {
	return ip >= (uintptr_t)stubsPtr && ip < (uintptr_t)stubsLimit;
}

void handlePageFault(FaultImageAccessor image, uintptr_t address);
void handleOtherFault(FaultImageAccessor image, Fault fault);
void handleIrq(IrqImageAccessor image, int number);

extern "C" void onPlatformFault(FaultImageAccessor image, int number) {
	Domain domain = determineDomain(*image.cs());
	assert(domain == kDomClientUser || domain == kDomClientSupervisor
			|| domain == kDomExecutorKernel);
	assert(!inStub(*image.ip()));

	if(domain == kDomClientUser || domain == kDomClientSupervisor)
		asm volatile ( "swapgs" : : : "memory" );

	switch(number) {
	case 3: {
		handleOtherFault(image, kFaultBreakpoint);
	} break;
	case 14: {
		uintptr_t address;
		asm volatile ( "mov %%cr2, %0" : "=r" (address) );
		handlePageFault(image, address);
	} break;
	default:
		frigg::panicLogger.log() << "Unexpected fault number " << number << frigg::EndLog();
	}
	
	if(domain == kDomClientUser || domain == kDomClientSupervisor)
		asm volatile ( "swapgs" : : : "memory" );
}

extern "C" void onPlatformIrq(IrqImageAccessor image, int number) {
	Domain domain = determineDomain(*image.cs());
	assert(domain == kDomClientUser || domain == kDomClientSupervisor
			|| domain == kDomExecutorKernel);
	assert(!inStub(*image.ip()));

	if(domain == kDomClientUser || domain == kDomClientSupervisor)
		asm volatile ( "swapgs" : : : "memory" );

	handleIrq(image, number);
	
	if(domain == kDomClientUser || domain == kDomClientSupervisor)
		asm volatile ( "swapgs" : : : "memory" );
}

bool intsAreEnabled() {
	uint64_t rflags;
	asm volatile ( "pushfq\n"
			"\rpop %0" : "=r" (rflags) );
	return (rflags & 0x200) != 0;
}

void enableInts() {
	asm volatile ( "sti" );
}

void disableInts() {
	asm volatile ( "cli" );
}

} // namespace thor

