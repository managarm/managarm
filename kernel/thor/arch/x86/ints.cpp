
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/profile.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch/pmc-amd.hpp>
#include <thor-internal/arch/pmc-intel.hpp>

extern char stubsPtr[], stubsLimit[];

extern "C" void earlyStubDivideByZero();
extern "C" void earlyStubOpcode();
extern "C" void earlyStubDouble();
extern "C" void earlyStubProtection();
extern "C" void earlyStubPage();

extern "C" void faultStubDivideByZero();
extern "C" void faultStubDebug();
extern "C" void faultStubBreakpoint();
extern "C" void faultStubOverflow();
extern "C" void faultStubBound();
extern "C" void faultStubOpcode();
extern "C" void faultStubNoFpu();
extern "C" void faultStubDouble();
extern "C" void faultStub9();
extern "C" void faultStubInvalidTss();
extern "C" void faultStubSegment();
extern "C" void faultStubStack();
extern "C" void faultStubProtection();
extern "C" void faultStubPage();
extern "C" void faultStub15();
extern "C" void faultStubFpuException();
extern "C" void faultStubAlignment();
extern "C" void faultStubMachineCheck();
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
extern "C" void thorRtIsrIrq24();
extern "C" void thorRtIsrIrq25();
extern "C" void thorRtIsrIrq26();
extern "C" void thorRtIsrIrq27();
extern "C" void thorRtIsrIrq28();
extern "C" void thorRtIsrIrq29();
extern "C" void thorRtIsrIrq30();
extern "C" void thorRtIsrIrq31();
extern "C" void thorRtIsrIrq32();
extern "C" void thorRtIsrIrq33();
extern "C" void thorRtIsrIrq34();
extern "C" void thorRtIsrIrq35();
extern "C" void thorRtIsrIrq36();
extern "C" void thorRtIsrIrq37();
extern "C" void thorRtIsrIrq38();
extern "C" void thorRtIsrIrq39();
extern "C" void thorRtIsrIrq40();
extern "C" void thorRtIsrIrq41();
extern "C" void thorRtIsrIrq42();
extern "C" void thorRtIsrIrq43();
extern "C" void thorRtIsrIrq44();
extern "C" void thorRtIsrIrq45();
extern "C" void thorRtIsrIrq46();
extern "C" void thorRtIsrIrq47();
extern "C" void thorRtIsrIrq48();
extern "C" void thorRtIsrIrq49();
extern "C" void thorRtIsrIrq50();
extern "C" void thorRtIsrIrq51();
extern "C" void thorRtIsrIrq52();
extern "C" void thorRtIsrIrq53();
extern "C" void thorRtIsrIrq54();
extern "C" void thorRtIsrIrq55();
extern "C" void thorRtIsrIrq56();
extern "C" void thorRtIsrIrq57();
extern "C" void thorRtIsrIrq58();
extern "C" void thorRtIsrIrq59();
extern "C" void thorRtIsrIrq60();
extern "C" void thorRtIsrIrq61();
extern "C" void thorRtIsrIrq62();
extern "C" void thorRtIsrIrq63();

extern "C" void thorRtIsrLegacyIrq7();
extern "C" void thorRtIsrLegacyIrq15();

extern "C" void thorRtIpiShootdown();
extern "C" void thorRtIpiPing();
extern "C" void thorRtPreemption();

extern "C" void nmiStub();

namespace thor {

static constexpr bool logEveryFault = false;
static constexpr bool logEveryPreemption = false;

uint32_t earlyGdt[3 * 2];
uint32_t earlyIdt[256 * 4];

extern "C" void handleEarlyDivideByZeroFault(void *rip) {
	panicLogger() << "Division by zero during boot\n"
			<< "Faulting IP: " << rip << frg::endlog;
}

extern "C" void handleEarlyOpcodeFault(void *rip) {
	panicLogger() << "Invalid opcode during boot\n"
			<< "Faulting IP: " << rip << frg::endlog;
}

extern "C" void handleEarlyDoubleFault(uint64_t errcode, void *rip) {
	(void)errcode;

	panicLogger() << "Double fault during boot\n"
			<< "Faulting IP: " << rip << frg::endlog;
}

extern "C" void handleEarlyProtectionFault(uint64_t errcode, void *rip) {
	panicLogger() << "Protection fault during boot\n"
			<< "Segment: " << errcode << "\n"
			<< "Faulting IP: " << rip << frg::endlog;
}

extern "C" void handleEarlyPageFault(uint64_t errcode, void *rip) {
	(void)errcode;
	uintptr_t pfAddress;
	asm volatile ("mov %%cr2, %0" : "=r" (pfAddress));

	panicLogger() << "Page fault at " << (void *)pfAddress << " during boot\n"
			<< "Faulting IP: " << rip << frg::endlog;
}

void setupEarlyInterruptHandlers() {
	// setup the gdt
	common::x86::makeGdtNullSegment(earlyGdt, 0);
	// for simplicity, match the layout with the "real" gdt we load later
	common::x86::makeGdtCode64SystemSegment(earlyGdt, 1);
	common::x86::makeGdtFlatData32SystemSegment(earlyGdt, 2);

	common::x86::Gdtr gdtr;
	gdtr.limit = 3 * 8;
	gdtr.pointer = earlyGdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq $0x8\n"
			"\rpushq $.L_reloadEarlyCs\n"
			"\rlretq\n"
			".L_reloadEarlyCs:" );
	
	// setup the idt
	common::x86::makeIdt64IntSystemGate(earlyIdt, 0, 0x8, (void *)&earlyStubDivideByZero, 0);
	common::x86::makeIdt64IntSystemGate(earlyIdt, 6, 0x8, (void *)&earlyStubOpcode, 0);
	common::x86::makeIdt64IntSystemGate(earlyIdt, 8, 0x8, (void *)&earlyStubDouble, 0);
	common::x86::makeIdt64IntSystemGate(earlyIdt, 13, 0x8, (void *)&earlyStubProtection, 0);
	common::x86::makeIdt64IntSystemGate(earlyIdt, 14, 0x8, (void *)&earlyStubPage, 0);
	
	common::x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = earlyIdt;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) : "memory" );
}

void setupIdt(uint32_t *table) {
	using common::x86::makeIdt64IntSystemGate;
	using common::x86::makeIdt64IntUserGate;
	
	int fault_selector = kSelExecutorFaultCode;
	makeIdt64IntSystemGate(table, 0, fault_selector, (void *)&faultStubDivideByZero, 0);
	makeIdt64IntSystemGate(table, 1, fault_selector, (void *)&faultStubDebug, 0);
	makeIdt64IntUserGate(table, 3, fault_selector, (void *)&faultStubBreakpoint, 0);
	makeIdt64IntSystemGate(table, 4, fault_selector, (void *)&faultStubOverflow, 0);
	makeIdt64IntSystemGate(table, 5, fault_selector, (void *)&faultStubBound, 0);
	makeIdt64IntSystemGate(table, 6, fault_selector, (void *)&faultStubOpcode, 0);
	makeIdt64IntSystemGate(table, 7, fault_selector, (void *)&faultStubNoFpu, 0);
	makeIdt64IntSystemGate(table, 8, fault_selector, (void *)&faultStubDouble, 2);
	makeIdt64IntSystemGate(table, 9, fault_selector, (void *)&faultStub9, 0);
	makeIdt64IntSystemGate(table, 10, fault_selector, (void *)&faultStubInvalidTss, 0);
	makeIdt64IntSystemGate(table, 11, fault_selector, (void *)&faultStubSegment, 0);
	makeIdt64IntSystemGate(table, 12, fault_selector, (void *)&faultStubStack, 0);
	makeIdt64IntSystemGate(table, 13, fault_selector, (void *)&faultStubProtection, 0);
	makeIdt64IntSystemGate(table, 14, fault_selector, (void *)&faultStubPage, 0);
	makeIdt64IntSystemGate(table, 15, fault_selector, (void *)&faultStub15, 0);
	makeIdt64IntSystemGate(table, 16, fault_selector, (void *)&faultStubFpuException, 0);
	makeIdt64IntSystemGate(table, 17, fault_selector, (void *)&faultStubAlignment, 0);
	makeIdt64IntSystemGate(table, 18, fault_selector, (void *)&faultStubMachineCheck, 0);
	makeIdt64IntSystemGate(table, 19, fault_selector, (void *)&faultStubSimdException, 0);

	int irq_selector = kSelSystemIrqCode;
	makeIdt64IntSystemGate(table, 39, irq_selector, (void *)&thorRtIsrLegacyIrq7, 1);
	makeIdt64IntSystemGate(table, 47, irq_selector, (void *)&thorRtIsrLegacyIrq15, 1);

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
	makeIdt64IntSystemGate(table, 88, irq_selector, (void *)&thorRtIsrIrq24, 1);
	makeIdt64IntSystemGate(table, 89, irq_selector, (void *)&thorRtIsrIrq25, 1);
	makeIdt64IntSystemGate(table, 90, irq_selector, (void *)&thorRtIsrIrq26, 1);
	makeIdt64IntSystemGate(table, 91, irq_selector, (void *)&thorRtIsrIrq27, 1);
	makeIdt64IntSystemGate(table, 92, irq_selector, (void *)&thorRtIsrIrq28, 1);
	makeIdt64IntSystemGate(table, 93, irq_selector, (void *)&thorRtIsrIrq29, 1);
	makeIdt64IntSystemGate(table, 94, irq_selector, (void *)&thorRtIsrIrq30, 1);
	makeIdt64IntSystemGate(table, 95, irq_selector, (void *)&thorRtIsrIrq31, 1);
	makeIdt64IntSystemGate(table, 96, irq_selector, (void *)&thorRtIsrIrq32, 1);
	makeIdt64IntSystemGate(table, 97, irq_selector, (void *)&thorRtIsrIrq33, 1);
	makeIdt64IntSystemGate(table, 98, irq_selector, (void *)&thorRtIsrIrq34, 1);
	makeIdt64IntSystemGate(table, 99, irq_selector, (void *)&thorRtIsrIrq35, 1);
	makeIdt64IntSystemGate(table, 100, irq_selector, (void *)&thorRtIsrIrq36, 1);
	makeIdt64IntSystemGate(table, 101, irq_selector, (void *)&thorRtIsrIrq37, 1);
	makeIdt64IntSystemGate(table, 102, irq_selector, (void *)&thorRtIsrIrq38, 1);
	makeIdt64IntSystemGate(table, 103, irq_selector, (void *)&thorRtIsrIrq39, 1);
	makeIdt64IntSystemGate(table, 104, irq_selector, (void *)&thorRtIsrIrq40, 1);
	makeIdt64IntSystemGate(table, 105, irq_selector, (void *)&thorRtIsrIrq41, 1);
	makeIdt64IntSystemGate(table, 106, irq_selector, (void *)&thorRtIsrIrq42, 1);
	makeIdt64IntSystemGate(table, 107, irq_selector, (void *)&thorRtIsrIrq43, 1);
	makeIdt64IntSystemGate(table, 108, irq_selector, (void *)&thorRtIsrIrq44, 1);
	makeIdt64IntSystemGate(table, 109, irq_selector, (void *)&thorRtIsrIrq45, 1);
	makeIdt64IntSystemGate(table, 110, irq_selector, (void *)&thorRtIsrIrq46, 1);
	makeIdt64IntSystemGate(table, 111, irq_selector, (void *)&thorRtIsrIrq47, 1);
	makeIdt64IntSystemGate(table, 112, irq_selector, (void *)&thorRtIsrIrq48, 1);
	makeIdt64IntSystemGate(table, 113, irq_selector, (void *)&thorRtIsrIrq49, 1);
	makeIdt64IntSystemGate(table, 114, irq_selector, (void *)&thorRtIsrIrq50, 1);
	makeIdt64IntSystemGate(table, 115, irq_selector, (void *)&thorRtIsrIrq51, 1);
	makeIdt64IntSystemGate(table, 116, irq_selector, (void *)&thorRtIsrIrq52, 1);
	makeIdt64IntSystemGate(table, 117, irq_selector, (void *)&thorRtIsrIrq53, 1);
	makeIdt64IntSystemGate(table, 118, irq_selector, (void *)&thorRtIsrIrq54, 1);
	makeIdt64IntSystemGate(table, 119, irq_selector, (void *)&thorRtIsrIrq55, 1);
	makeIdt64IntSystemGate(table, 120, irq_selector, (void *)&thorRtIsrIrq56, 1);
	makeIdt64IntSystemGate(table, 121, irq_selector, (void *)&thorRtIsrIrq57, 1);
	makeIdt64IntSystemGate(table, 122, irq_selector, (void *)&thorRtIsrIrq58, 1);
	makeIdt64IntSystemGate(table, 123, irq_selector, (void *)&thorRtIsrIrq59, 1);
	makeIdt64IntSystemGate(table, 124, irq_selector, (void *)&thorRtIsrIrq60, 1);
	makeIdt64IntSystemGate(table, 125, irq_selector, (void *)&thorRtIsrIrq61, 1);
	makeIdt64IntSystemGate(table, 126, irq_selector, (void *)&thorRtIsrIrq62, 1);
	makeIdt64IntSystemGate(table, 127, irq_selector, (void *)&thorRtIsrIrq63, 1);
	
	makeIdt64IntSystemGate(table, 0xF0, irq_selector, (void *)&thorRtIpiShootdown, 1);
	makeIdt64IntSystemGate(table, 0xF1, irq_selector, (void *)&thorRtIpiPing, 1);
	makeIdt64IntSystemGate(table, 0xFF, irq_selector, (void *)&thorRtPreemption, 1);
	
	int nmi_selector = kSelSystemNmiCode;
	makeIdt64IntSystemGate(table, 2, nmi_selector, (void *)&nmiStub, 3);

	//FIXME
//	common::x86::makeIdt64IntSystemGate(table, 0x82,
//			0x8, (void *)&thorRtIsrPreempted, 0);
}

bool inStub(uintptr_t ip) {
	return ip >= (uintptr_t)stubsPtr && ip < (uintptr_t)stubsLimit;
}

void handlePageFault(FaultImageAccessor image, uintptr_t address, Word errorCode);
void handleOtherFault(FaultImageAccessor image, Interrupt fault);
void handleIrq(IrqImageAccessor image, int number);
void handlePreemption(IrqImageAccessor image);
void handleSyscall(SyscallImageAccessor image);

void handleDebugFault(FaultImageAccessor image) {
	debugLogger() << "thor: Debug fault at ip: " << (void *)*image.ip() << frg::endlog;
}

extern "C" void onPlatformFault(FaultImageAccessor image, int number) {
	// For page faults: we need to get the address *before* re-enabling IRQs.
	uintptr_t pfAddress;
	if(number == 14)
		asm volatile ("mov %%cr2, %0" : "=r" (pfAddress));

	enableInts();

	uint16_t cs = *image.cs();
	if(logEveryFault)
		infoLogger() << "Fault #" << number << ", from cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	if(inStub(*image.ip()))
		panicLogger() << "Fault #" << number
				<< " in stub section, cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << frg::endlog;
	if(cs != kSelSystemIrqCode && cs != kSelClientUserCode
			&& cs != kSelExecutorFaultCode && cs != kSelExecutorSyscallCode)
		panicLogger() << "Fault #" << number
				<< ", from unexpected cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << "\n"
				<< "Error code: 0x" << frg::hex_fmt(*image.code())
				<< ", SS: 0x" << frg::hex_fmt(*image.ss())
				<< ", RSP: " << (void *)*image.sp() << frg::endlog;
	if(!(*image.rflags() & 0x200))
		panicLogger() << "Fault #" << number
				<< ", with IF=0, cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << "\n"
				<< "Error code: 0x" << frg::hex_fmt(*image.code())
				<< ", SS: 0x" << frg::hex_fmt(*image.ss())
				<< ", RSP: " << (void *)*image.sp() << frg::endlog;

	disableUserAccess();

	switch(number) {
	case 0: {
		handleOtherFault(image, kIntrDivByZero);
	} break;
	case 1: {
		handleDebugFault(image);
	} break;
	case 3: {
		handleOtherFault(image, kIntrBreakpoint);
	} break;
	case 6: {
		handleOtherFault(image, kIntrIllegalInstruction);
	} break;
	case 13: {
		handleOtherFault(image, kIntrGeneralFault);
	} break;
	case 14: {
		handlePageFault(image, pfAddress, *image.code());
	} break;
	default:
		panicLogger() << "Unexpected fault number " << number
				<< ", from cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << "\n"
				<< "Error code: 0x" << frg::hex_fmt(*image.code())
				<< ", SS: 0x" << frg::hex_fmt(*image.ss())
				<< ", RSP: " << (void *)*image.sp() << frg::endlog;
	}

	disableInts();
}

extern "C" void onPlatformIrq(IrqImageAccessor image, int number) {
	if(inStub(*image.ip()))
		panicLogger() << "IRQ " << number
				<< " in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	uint16_t cs = *image.cs();
	assert(cs == kSelSystemIdleCode || cs == kSelSystemFiberCode
			|| cs == kSelClientUserCode || cs == kSelExecutorSyscallCode
			|| cs == kSelExecutorFaultCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	handleIrq(image, number);
}

extern "C" void onPlatformLegacyIrq(IrqImageAccessor image, int number) {
	if(inStub(*image.ip()))
		panicLogger() << "IRQ " << number
				<< " in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	uint16_t cs = *image.cs();
	assert(cs == kSelSystemIdleCode || cs == kSelSystemFiberCode
			|| cs == kSelClientUserCode || cs == kSelExecutorSyscallCode
			|| cs == kSelExecutorFaultCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	if(checkLegacyPicIsr(number)) {
		urgentLogger() << "thor: Spurious IRQ " << number
				<< " of legacy PIC" << frg::endlog;
	}else{
		urgentLogger() << "thor: Ignoring non-spurious IRQ " << number
				<< " of legacy PIC" << frg::endlog;
	}
}

extern "C" void onPlatformPreemption(IrqImageAccessor image) {
	if(inStub(*image.ip()))
		panicLogger() << "Preemption IRQ"
				" in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;
	
	uint16_t cs = *image.cs();
	if(logEveryPreemption)
		infoLogger() << "thor [CPU " << getLocalApicId()
				<< "]: Preemption from cs: 0x" << frg::hex_fmt(cs)
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	assert(cs == kSelSystemIdleCode || cs == kSelSystemFiberCode
			|| cs == kSelClientUserCode || cs == kSelExecutorSyscallCode
			|| cs == kSelExecutorFaultCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	LocalApicContext::handleTimerIrq();

	getCpuData()->heartbeat.fetch_add(1, std::memory_order_relaxed);

	acknowledgeIrq(0);

	handlePreemption(image);
}

extern "C" void onPlatformSyscall(SyscallImageAccessor image) {
	assert(!irqMutex().nesting());
	enableInts();
	// TODO: User-access should already be disabled here.
	disableUserAccess();

	handleSyscall(image);

	disableInts();
}

extern "C" void onPlatformShootdown(IrqImageAccessor image) {
	if(inStub(*image.ip()))
		panicLogger() << "Shootdown IPI"
				<< " in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	uint16_t cs = *image.cs();
	assert(cs == kSelSystemIdleCode || cs == kSelSystemFiberCode
			|| cs == kSelClientUserCode || cs == kSelExecutorSyscallCode
			|| cs == kSelExecutorFaultCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	for(int i = 0; i < maxPcidCount; i++)
		getCpuData()->pcidBindings[i].shootdown();

	getCpuData()->globalBinding.shootdown();

	acknowledgeIpi();
}

extern "C" void onPlatformPing(IrqImageAccessor image) {
	if(inStub(*image.ip()))
		panicLogger() << "Ping IPI"
				<< " in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	uint16_t cs = *image.cs();
	assert(cs == kSelSystemIdleCode || cs == kSelSystemFiberCode
			|| cs == kSelClientUserCode || cs == kSelExecutorSyscallCode
			|| cs == kSelExecutorFaultCode);

	assert(!irqMutex().nesting());
	disableUserAccess();

	acknowledgeIpi();

	handlePreemption(image);
}

extern "C" void onPlatformWork() {
//	if(inStub(*image.ip()))
//		panicLogger() << "Work interrupt " << number
//				<< " in stub section, cs: 0x" << frg::hex_fmt(*image.cs())
//				<< ", ip: " << (void *)*image.ip() << frg::endlog;

	assert(!irqMutex().nesting());
	// TODO: User-access should already be disabled here.
	disableUserAccess();

	enableInts();
	getCurrentThread()->mainWorkQueue()->run();
	disableInts();
}

extern "C" void onPlatformNmi(NmiImageAccessor image) {
	// If we interrupted user space or a kernel stub, we might need to update GS.
	auto gs = common::x86::rdmsr(common::x86::kMsrIndexGsBase);
	common::x86::wrmsr(common::x86::kMsrIndexGsBase,
			reinterpret_cast<uintptr_t>(*image.expectedGs()));

	auto cpuData = getCpuData();

	bool explained = false;
	auto pmcMechanism = cpuData->profileMechanism.load(std::memory_order_acquire);
	if(pmcMechanism == ProfileMechanism::intelPmc && checkIntelPmcOverflow()) {
		uintptr_t ip = *image.ip();
		cpuData->localProfileRing->enqueue(&ip, sizeof(uintptr_t));
		setIntelPmc();
		explained = true;
	}else if(pmcMechanism == ProfileMechanism::amdPmc && checkAmdPmcOverflow()) {
		uintptr_t ip = *image.ip();
		cpuData->localProfileRing->enqueue(&ip, sizeof(uintptr_t));
		setAmdPmc();
		explained = true;
	}

	if(!explained) {
		infoLogger() << "thor [CPU " << getLocalApicId()
				<< "]: NMI triggered at heartbeat "
				<< cpuData->heartbeat.load(std::memory_order_relaxed) << frg::endlog;
		infoLogger() << "thor [CPU " << getLocalApicId()
				<< "]: From CS: 0x" << frg::hex_fmt(*image.cs())
				<< ", IP: " << (void *)*image.ip() << frg::endlog;
		infoLogger() << "thor [CPU " << getLocalApicId()
				<< "]: RFLAGS is " << (void *)*image.rflags() << frg::endlog;

		if(!getLocalApicId())
			sendGlobalNmi();
	}

	// Restore the old value of GS.
	common::x86::wrmsr(common::x86::kMsrIndexGsBase,
			reinterpret_cast<uintptr_t>(gs));
}

extern "C" void enableIntsAndHaltForever();

void suspendSelf() {
	assert(!intsAreEnabled());
	enableIntsAndHaltForever();
}

} // namespace thor

