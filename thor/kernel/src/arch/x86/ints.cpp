
#include "generic/kernel.hpp"

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
extern "C" void faultStubFpuException();
extern "C" void faultStubSimdException();

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
extern "C" void thorRtIsrIrq16();
extern "C" void thorRtIsrIrq17();
extern "C" void thorRtIsrIrq18();
extern "C" void thorRtIsrIrq19();
extern "C" void thorRtIsrIrq20();
extern "C" void thorRtIsrIrq21();
extern "C" void thorRtIsrIrq22();
extern "C" void thorRtIsrIrq23();

extern "C" void thorRtIpiShootdown();
extern "C" void thorRtIpiPing();
extern "C" void thorRtPreemption();

namespace thor {

static constexpr bool logEveryFault = false;

uint32_t earlyGdt[3 * 2];
uint32_t earlyIdt[256 * 4];

extern "C" void handleEarlyDivideByZeroFault(void *rip) {
	frigg::panicLogger() << "Division by zero during boot\n"
			<< "Faulting IP: " << rip << frigg::endLog;
}

extern "C" void handleEarlyOpcodeFault(void *rip) {
	frigg::panicLogger() << "Invalid opcode during boot\n"
			<< "Faulting IP: " << rip << frigg::endLog;
}

extern "C" void handleEarlyDoubleFault(uint64_t errcode, void *rip) {
	(void)errcode;

	frigg::panicLogger() << "Double fault during boot\n"
			<< "Faulting IP: " << rip << frigg::endLog;
}

extern "C" void handleEarlyProtectionFault(uint64_t errcode, void *rip) {
	frigg::panicLogger() << "Protection fault during boot\n"
			<< "Segment: " << errcode << "\n"
			<< "Faulting IP: " << rip << frigg::endLog;
}

extern "C" void handleEarlyPageFault(uint64_t errcode, void *rip) {
	(void)errcode;

	frigg::panicLogger() << "Page fault during boot\n"
			<< "Faulting IP: " << rip << frigg::endLog;
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
	using frigg::arch_x86::makeIdt64IntSystemGate;
	using frigg::arch_x86::makeIdt64IntUserGate;
	
	int fault_selector = kSelExecutorFaultCode;
	makeIdt64IntSystemGate(table, 0, fault_selector, (void *)&faultStubDivideByZero, 0);
	makeIdt64IntSystemGate(table, 1, fault_selector, (void *)&faultStubDebug, 0);
	makeIdt64IntUserGate(table, 3, fault_selector, (void *)&faultStubBreakpoint, 0);
	makeIdt64IntSystemGate(table, 6, fault_selector, (void *)&faultStubOpcode, 0);
	makeIdt64IntSystemGate(table, 7, fault_selector, (void *)&faultStubNoFpu, 0);
	makeIdt64IntSystemGate(table, 8, fault_selector, (void *)&faultStubDouble, 0);
	makeIdt64IntSystemGate(table, 13, fault_selector, (void *)&faultStubProtection, 0);
	makeIdt64IntSystemGate(table, 14, fault_selector, (void *)&faultStubPage, 0);
	makeIdt64IntSystemGate(table, 16, fault_selector, (void *)&faultStubFpuException, 0);
	makeIdt64IntSystemGate(table, 19, fault_selector, (void *)&faultStubSimdException, 0);

	int irq_selector = kSelSystemIrqCode;
	makeIdt64IntSystemGate(table, 64, irq_selector, (void *)&thorRtIsrIrq0, 1);
	makeIdt64IntSystemGate(table, 65, irq_selector, (void *)&thorRtIsrIrq1, 1);
	makeIdt64IntSystemGate(table, 66, irq_selector, (void *)&thorRtIsrIrq2, 1);
	makeIdt64IntSystemGate(table, 67, irq_selector, (void *)&thorRtIsrIrq3, 1);
	makeIdt64IntSystemGate(table, 68, irq_selector, (void *)&thorRtIsrIrq4, 1);
	makeIdt64IntSystemGate(table, 69, irq_selector, (void *)&thorRtIsrIrq5, 1);
	makeIdt64IntSystemGate(table, 70, irq_selector, (void *)&thorRtIsrIrq6, 1);
	makeIdt64IntSystemGate(table, 71, irq_selector, (void *)&thorRtIsrIrq7, 1);
	makeIdt64IntSystemGate(table, 72, irq_selector, (void *)&thorRtIsrIrq8, 1);
	makeIdt64IntSystemGate(table, 73, irq_selector, (void *)&thorRtIsrIrq9, 1);
	makeIdt64IntSystemGate(table, 74, irq_selector, (void *)&thorRtIsrIrq10, 1);
	makeIdt64IntSystemGate(table, 75, irq_selector, (void *)&thorRtIsrIrq11, 1);
	makeIdt64IntSystemGate(table, 76, irq_selector, (void *)&thorRtIsrIrq12, 1);
	makeIdt64IntSystemGate(table, 77, irq_selector, (void *)&thorRtIsrIrq13, 1);
	makeIdt64IntSystemGate(table, 78, irq_selector, (void *)&thorRtIsrIrq14, 1);
	makeIdt64IntSystemGate(table, 79, irq_selector, (void *)&thorRtIsrIrq15, 1);
	makeIdt64IntSystemGate(table, 80, irq_selector, (void *)&thorRtIsrIrq16, 1);
	makeIdt64IntSystemGate(table, 81, irq_selector, (void *)&thorRtIsrIrq17, 1);
	makeIdt64IntSystemGate(table, 82, irq_selector, (void *)&thorRtIsrIrq18, 1);
	makeIdt64IntSystemGate(table, 83, irq_selector, (void *)&thorRtIsrIrq19, 1);
	makeIdt64IntSystemGate(table, 84, irq_selector, (void *)&thorRtIsrIrq20, 1);
	makeIdt64IntSystemGate(table, 85, irq_selector, (void *)&thorRtIsrIrq21, 1);
	makeIdt64IntSystemGate(table, 86, irq_selector, (void *)&thorRtIsrIrq22, 1);
	makeIdt64IntSystemGate(table, 87, irq_selector, (void *)&thorRtIsrIrq23, 1);
	
	makeIdt64IntSystemGate(table, 0xF0, irq_selector, (void *)&thorRtIpiShootdown, 1);
	makeIdt64IntSystemGate(table, 0xF1, irq_selector, (void *)&thorRtIpiPing, 1);
	makeIdt64IntSystemGate(table, 0xFF, irq_selector, (void *)&thorRtPreemption, 1);

	//FIXME
//	frigg::arch_x86::makeIdt64IntSystemGate(table, 0x82,
//			0x8, (void *)&thorRtIsrPreempted, 0);
}

bool inStub(uintptr_t ip) {
	return ip >= (uintptr_t)stubsPtr && ip < (uintptr_t)stubsLimit;
}

void handlePageFault(FaultImageAccessor image, uintptr_t address);
void handleOtherFault(FaultImageAccessor image, Interrupt fault);
void handleIrq(IrqImageAccessor image, int number);
void handlePreemption(IrqImageAccessor image);
void handleSyscall(SyscallImageAccessor image);

void handleDebugFault(FaultImageAccessor image) {
	frigg::infoLogger() << "\e[35mthor: Debug fault "
			<< "at ip: " << (void *)*image.ip() << "\e[39m" << frigg::endLog;
}

extern "C" void onPlatformFault(FaultImageAccessor image, int number) {
	uint16_t cs = *image.cs();

	if(logEveryFault)
		frigg::infoLogger() << "Fault #" << number << ", from cs: 0x" << frigg::logHex(cs)
				<< ", ip: " << (void *)*image.ip() << frigg::endLog;

	if(inStub(*image.ip()))
		frigg::panicLogger() << "Fault #" << number
				<< " in stub section, cs: 0x" << frigg::logHex(cs)
				<< ", ip: " << (void *)*image.ip() << frigg::endLog;
	if(cs != kSelSystemIrqCode && cs != kSelClientUserCode
			&& cs != kSelExecutorFaultCode && cs != kSelExecutorSyscallCode)
		frigg::panicLogger() << "Fault #" << number
				<< ", from unexpected cs: 0x" << frigg::logHex(cs)
				<< ", ip: " << (void *)*image.ip() << frigg::endLog;

	disableUserAccess();

	switch(number) {
	case 1: {
		handleDebugFault(image);
	} break;
	case 3: {
		handleOtherFault(image, kIntrBreakpoint);
	} break;
	case 13: {
		handleOtherFault(image, kIntrGeneralFault);
	} break;
	case 14: {
		uintptr_t address;
		asm volatile ( "mov %%cr2, %0" : "=r" (address) );
		handlePageFault(image, address);
	} break;
	default:
		frigg::panicLogger() << "Unexpected fault number " << number
				<< ", from cs: 0x" << frigg::logHex(cs)
				<< ", ip: " << (void *)*image.ip() << frigg::endLog;
	}
}

extern "C" void onPlatformIrq(IrqImageAccessor image, int number) {
	if(inStub(*image.ip()))
		frigg::panicLogger() << "IRQ " << number
				<< " in stub section, cs: 0x" << frigg::logHex(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frigg::endLog;

	uint16_t cs = *image.cs();
	assert(cs == kSelSystemIdleCode || cs == kSelSystemFiberCode
			|| cs == kSelClientUserCode || cs == kSelExecutorSyscallCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	handleIrq(image, number);
}

extern "C" void onPlatformPreemption(IrqImageAccessor image) {
	if(inStub(*image.ip()))
		frigg::panicLogger() << "Preemption IRQ"
				" in stub section, cs: 0x" << frigg::logHex(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frigg::endLog;

	uint16_t cs = *image.cs();
	assert(cs == kSelSystemIdleCode || cs == kSelSystemFiberCode
			|| cs == kSelClientUserCode || cs == kSelExecutorSyscallCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	LocalApicContext::handleTimerIrq();

	acknowledgeIrq(0);

	handlePreemption(image);
}

extern "C" void onPlatformSyscall(SyscallImageAccessor image) {
	assert(!irqMutex().nesting());
	enableInts();
	disableUserAccess();

	handleSyscall(image);

	disableInts();
}

extern "C" void onPlatformShootdown(IrqImageAccessor image) {
	if(inStub(*image.ip()))
		frigg::panicLogger() << "Shootdown IPI"
				<< " in stub section, cs: 0x" << frigg::logHex(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frigg::endLog;

	uint16_t cs = *image.cs();
	assert(cs == kSelSystemIdleCode || cs == kSelSystemFiberCode
			|| cs == kSelClientUserCode || cs == kSelExecutorSyscallCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	for(int i = 0; i < maxPcidCount; i++)
		getCpuData()->pcidBindings[i].shootdown();

	acknowledgeIpi();
}

extern "C" void onPlatformPing(IrqImageAccessor image) {
	if(inStub(*image.ip()))
		frigg::panicLogger() << "Shootdown IPI"
				<< " in stub section, cs: 0x" << frigg::logHex(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frigg::endLog;

	uint16_t cs = *image.cs();
	assert(cs == kSelSystemIdleCode || cs == kSelSystemFiberCode
			|| cs == kSelClientUserCode || cs == kSelExecutorSyscallCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	acknowledgeIpi();

	handlePreemption(image);
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

extern "C" void enableIntsAndHaltForever();

void suspendSelf() {
	assert(!intsAreEnabled());
	enableIntsAndHaltForever();
}

} // namespace thor

