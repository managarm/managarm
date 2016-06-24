
#include "../kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Debugging functions
// --------------------------------------------------------

void BochsSink::print(char c) {
	frigg::arch_x86::ioOutByte(0xE9, c);
}
void BochsSink::print(const char *str) {
	while(*str != 0)
		frigg::arch_x86::ioOutByte(0xE9, *str++);
}

// --------------------------------------------------------
// ExecutorImagePtr
// --------------------------------------------------------

ExecutorImagePtr ExecutorImagePtr::make() {
	return ExecutorImagePtr((char *)kernelAlloc->allocate(getStateSize()));
}

void saveExecutorFromIrq(IrqImagePtr base) {

}

// --------------------------------------------------------
// ThorRtThreadState
// --------------------------------------------------------

ThorRtThreadState::ThorRtThreadState()
: image(ExecutorImagePtr::make()), fsBase(0) {
	memset(&threadTss, 0, sizeof(frigg::arch_x86::Tss64));
	frigg::arch_x86::initializeTss64(&threadTss);
	threadTss.rsp0 = uintptr_t(syscallStack + kSyscallStackSize);
}

ThorRtThreadState::~ThorRtThreadState() {
	//FIXME: free the executor image ptr
}

void ThorRtThreadState::activate() {
	// set the current general / syscall state pointer
	asm volatile ( "mov %0, %%gs:%c1" : : "r" (image),
			"i" (ThorRtKernelGs::kOffExecutorImage) : "memory" );
	asm volatile ( "mov %0, %%gs:%c1"
			: : "r" (syscallStack + kSyscallStackSize),
			"i" (ThorRtKernelGs::kOffSyscallStackPtr) : "memory" );
	
	// setup the thread's tss segment
	ThorRtCpuSpecific *cpu_specific;
	asm volatile ( "mov %%gs:%c1, %0" : "=r" (cpu_specific)
			: "i" (ThorRtKernelGs::kOffCpuSpecific) );
	threadTss.ist1 = cpu_specific->tssTemplate.ist1;
	
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_specific->gdt, 6,
			&threadTss, sizeof(frigg::arch_x86::Tss64));
	asm volatile ( "ltr %w0" : : "r" ( 0x30 ) );

	// restore the fs segment limit
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, fsBase);
}

void ThorRtThreadState::deactivate() {
	// reset the current general / syscall state pointer
	asm volatile ( "mov %0, %%gs:%c1" : : "r" (nullptr),
			"i" (ThorRtKernelGs::kOffExecutorImage) : "memory" );
	asm volatile ( "mov %0, %%gs:%c1" : : "r" (nullptr),
			"i" (ThorRtKernelGs::kOffSyscallStackPtr) : "memory" );
	
	// setup the tss segment
	ThorRtCpuSpecific *cpu_specific;
	asm volatile ( "mov %%gs:%c1, %0" : "=r" (cpu_specific)
			: "i" (ThorRtKernelGs::kOffCpuSpecific) );
	
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_specific->gdt, 6,
			&cpu_specific->tssTemplate, sizeof(frigg::arch_x86::Tss64));
	asm volatile ( "ltr %w0" : : "r" ( 0x30 ) );

	// save the fs segment limit
	fsBase = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexFsBase);
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, 0);
}

// --------------------------------------------------------
// ThorRtKernelGs
// --------------------------------------------------------

ThorRtKernelGs::ThorRtKernelGs()
: cpuContext(nullptr), stateSize(0), syscallStackPtr(nullptr),
		cpuSpecific(nullptr) { }

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

size_t getStateSize() {
	size_t result;
	asm volatile ( "mov %%gs:%c1, %0" : "=r" (result)
			: "i" (ThorRtKernelGs::kOffStateSize) );
	return result;
}

CpuContext *getCpuContext() {
	CpuContext *context;
	asm volatile ( "mov %%gs:%c1, %0" : "=r" (context)
			: "i" (ThorRtKernelGs::kOffCpuContext) );
	return context;
}

void callOnCpuStack(void (*function) ()) {
	assert(!intsAreEnabled());

	ThorRtCpuSpecific *cpu_specific;
	asm volatile ( "mov %%gs:%c1, %0" : "=r" (cpu_specific)
			: "i" (ThorRtKernelGs::kOffCpuSpecific) );
	
	uintptr_t stack_ptr = (uintptr_t)cpu_specific->cpuStack
			+ ThorRtCpuSpecific::kCpuStackSize;
	
	asm volatile ( "mov %0, %%rsp\n"
			"\tcall *%1\n"
			"\tud2\n" : : "r" (stack_ptr), "r" (function) );
	__builtin_unreachable();
}

extern "C" void syscallStub();

void initializeThisProcessor() {
	auto cpu_specific = frigg::construct<ThorRtCpuSpecific>(*kernelAlloc);
	
	// set up the kernel gs segment
	auto kernel_gs = frigg::construct<ThorRtKernelGs>(*kernelAlloc);
	kernel_gs->stateSize = sizeof(GprState) + sizeof(FxState);
	kernel_gs->flags = 0;
	kernel_gs->cpuSpecific = cpu_specific;
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexGsBase, (uintptr_t)kernel_gs);
	
	// set up the cpu context. we do this after setting up gs because
	// the CpuContext constructor calls getStateSize()
	kernel_gs->cpuContext = frigg::construct<CpuContext>(*kernelAlloc);

	// setup the gdt
	// note: the tss requires two slots in the gdt
	frigg::arch_x86::makeGdtNullSegment(cpu_specific->gdt, 0);
	// the layout of the next two kernel descriptors is forced by the use of sysret
	frigg::arch_x86::makeGdtCode64SystemSegment(cpu_specific->gdt, 1);
	frigg::arch_x86::makeGdtFlatData32SystemSegment(cpu_specific->gdt, 2);
	// the layout of the next three user-space descriptors is forced by the use of sysret
	frigg::arch_x86::makeGdtNullSegment(cpu_specific->gdt, 3);
	frigg::arch_x86::makeGdtFlatData32UserSegment(cpu_specific->gdt, 4);
	frigg::arch_x86::makeGdtCode64UserSegment(cpu_specific->gdt, 5);
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_specific->gdt, 6, nullptr, 0);

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 8 * 8;
	gdtr.pointer = cpu_specific->gdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq $0x8\n"
			"\rpushq $.L_reloadCs\n"
			"\rlretq\n"
			".L_reloadCs:" );
	
	// setup a stack for irqs
	size_t irq_stack_size = 0x10000;
	void *irq_stack_base = kernelAlloc->allocate(irq_stack_size);
	
	// setup the kernel tss
	frigg::arch_x86::initializeTss64(&cpu_specific->tssTemplate);
	cpu_specific->tssTemplate.ist1 = (uintptr_t)irq_stack_base + irq_stack_size;
	
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_specific->gdt, 6,
			&cpu_specific->tssTemplate, sizeof(frigg::arch_x86::Tss64));
	asm volatile ( "ltr %w0" : : "r" ( 0x30 ) );
	
	// setup the idt
	for(int i = 0; i < 256; i++)
		frigg::arch_x86::makeIdt64NullGate(cpu_specific->idt, i);
	setupIdt(cpu_specific->idt);

	frigg::arch_x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = cpu_specific->idt;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) );

	// enable wrfsbase / wrgsbase instructions
	// FIXME: does not seem to work under qemu
//	if(!(frigg::arch_x86::cpuid(frigg::arch_x86::kCpuIndexStructuredExtendedFeaturesEnum)[1]
//			& frigg::arch_x86::kCpuFlagFsGsBase))
//		frigg::panicLogger.log() << "CPU does not support wrfsbase / wrgsbase"
//				<< frigg::EndLog();
	
//	uint64_t cr4;
//	asm volatile ( "mov %%cr4, %0" : "=r" (cr4) );
//	cr4 |= 0x10000;
//	asm volatile ( "mov %0, %%cr4" : : "r" (cr4) );

	// setup the syscall interface
	if((frigg::arch_x86::cpuid(frigg::arch_x86::kCpuIndexExtendedFeatures)[3]
			& frigg::arch_x86::kCpuFlagSyscall) == 0)
		frigg::panicLogger.log() << "CPU does not support the syscall instruction"
				<< frigg::EndLog();
	
	uint64_t efer = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrEfer);
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrEfer,
			efer | frigg::arch_x86::kMsrSyscallEnable);

	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrLstar, (uintptr_t)&syscallStub);
	// user mode cs = 0x18, kernel mode cs = 0x08
	// set user mode rpl bits to work around a qemu bug
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrStar,
			(uint64_t(0x1B) << 48) | (uint64_t(0x08) << 32));
	// mask interrupt and trap flag
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrFmask, 0x300);

	initLocalApicPerCpu();
}

// note: these symbols have PHYSICAL addresses!
extern "C" void trampoline();
extern "C" uint32_t trampolineStatus;
extern "C" uint32_t trampolinePml4;
extern "C" uint64_t trampolineStack;

// generated by the linker script
extern "C" uint8_t _trampoline_startLma[];
extern "C" uint8_t _trampoline_endLma[];

bool secondaryBootComplete;
bool finishedBoot;

extern "C" void thorRtSecondaryEntry() {
	// inform the bsp that we do not need the trampoline area anymore
	frigg::volatileWrite<bool>(&secondaryBootComplete, true);

	infoLogger->log() << "Hello world from CPU #" << getLocalApicId() << frigg::EndLog();	
	initializeThisProcessor();

	infoLogger->log() << "Start scheduling on AP" << frigg::EndLog();
	ScheduleGuard schedule_guard(scheduleLock.get());
	doSchedule(frigg::move(schedule_guard));
}

void bootSecondary(uint32_t secondary_apic_id) {
	// copy the trampoline code into low physical memory
	uintptr_t trampoline_addr = (uintptr_t)trampoline;
	size_t trampoline_size = (uintptr_t)_trampoline_endLma - (uintptr_t)_trampoline_startLma;
	assert((trampoline_addr % 0x1000) == 0);
	assert((trampoline_size % 0x1000) == 0);
	memcpy(physicalToVirtual(trampoline_addr), _trampoline_startLma, trampoline_size);
	
	size_t trampoline_stack_size = 0x10000;
	void *trampoline_stack_base = kernelAlloc->allocate(trampoline_stack_size);

	// setup the trampoline data area
	auto status_ptr = accessPhysical<uint32_t>((PhysicalAddr)&trampolineStatus);
	auto pml4_ptr = accessPhysical<uint32_t>((PhysicalAddr)&trampolinePml4);
	auto stack_ptr = accessPhysical<uint64_t>((PhysicalAddr)&trampolineStack);
	secondaryBootComplete = false;
	*pml4_ptr = kernelSpace->getPml4();
	*stack_ptr = ((uintptr_t)trampoline_stack_base + trampoline_stack_size);

	raiseInitAssertIpi(secondary_apic_id);
	raiseInitDeassertIpi(secondary_apic_id);
	raiseStartupIpi(secondary_apic_id, trampoline_addr);
	asm volatile ( "" : : : "memory" );
	
	// wait until the ap wakes up
	infoLogger->log() << "Waiting for AP to wake up" << frigg::EndLog();
	while(frigg::volatileRead<uint32_t>(status_ptr) == 0) {
		frigg::pause();
	}
	
	// allow ap code to initialize the processor
	infoLogger->log() << "AP is booting" << frigg::EndLog();
	frigg::volatileWrite<uint32_t>(status_ptr, 2);
	
	// wait until the secondary processor completed its boot process
	// we can re-use the trampoline area after this completes
	while(!frigg::volatileRead<bool>(&secondaryBootComplete)) {
		frigg::pause();
	}
	infoLogger->log() << "AP finished booting" << frigg::EndLog();
}

} // namespace thor

