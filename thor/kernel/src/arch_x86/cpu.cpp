
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
// UniqueKernelStack
// --------------------------------------------------------

UniqueKernelStack UniqueKernelStack::make() {
	return UniqueKernelStack((char *)kernelAlloc->allocate(kSize));
}

// --------------------------------------------------------
// ExecutorImagePtr
// --------------------------------------------------------

size_t ExecutorImagePtr::determineSize() {
	return sizeof(General) + sizeof(FxState);
}

ExecutorImagePtr ExecutorImagePtr::make() {
	return ExecutorImagePtr((char *)kernelAlloc->allocate(getStateSize()));
}

void saveExecutorFromIrq(IrqImagePtr base) {

}

// --------------------------------------------------------
// PlatformExecutor
// --------------------------------------------------------

PlatformExecutor::PlatformExecutor()
: AssemblyExecutor(ExecutorImagePtr::make(), UniqueKernelStack::make()), fsBase(0) {
	memset(&threadTss, 0, sizeof(frigg::arch_x86::Tss64));
	frigg::arch_x86::initializeTss64(&threadTss);
	threadTss.rsp0 = (uintptr_t)kernelStack.base();
}

void enterExecutor(frigg::UnsafePtr<Thread> executor) {
	assert(!intsAreEnabled());

	AssemblyCpuContext *context = getCpuContext();
	assert(!context->activeExecutor);
	context->activeExecutor = executor;
	context->executorImage = executor->image;
	context->syscallStackPtr = executor->kernelStack.base();
	
	executor->getAddressSpace()->activate();


	// setup the thread's tss segment
	CpuContext *cpu_context = getCpuContext();
	executor->threadTss.ist1 = cpu_context->tssTemplate.ist1;
	
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_context->gdt, 6,
			&executor->threadTss, sizeof(frigg::arch_x86::Tss64));
	asm volatile ( "ltr %w0" : : "r" ( 0x30 ) );

	// restore the fs segment limit
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, executor->fsBase);
}

void exitExecutor() {
	assert(!intsAreEnabled());

	AssemblyCpuContext *context = getCpuContext();
	assert(context->activeExecutor);
	context->activeExecutor = frigg::UnsafePtr<AssemblyExecutor>();
	context->executorImage = ExecutorImagePtr();
	context->syscallStackPtr = nullptr;
	
	// setup the tss segment
	CpuContext *cpu_context = getCpuContext();
	
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_context->gdt, 6,
			&cpu_context->tssTemplate, sizeof(frigg::arch_x86::Tss64));
	asm volatile ( "ltr %w0" : : "r" ( 0x30 ) );

	// save the fs segment limit
	//FIXME: save / restore fsBase
	// executor->fsBase = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexFsBase);
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, 0);
}

frigg::UnsafePtr<Thread> activeExecutor() {
	return frigg::staticPtrCast<Thread>(getCpuContext()->activeExecutor);
}

// --------------------------------------------------------
// AssemblyCpuContext
// --------------------------------------------------------

AssemblyCpuContext::AssemblyCpuContext()
: syscallStackPtr(nullptr) { }

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

size_t getStateSize() {
	return ExecutorImagePtr::determineSize();
}

CpuContext *getCpuContext() {
	uint64_t msr = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexGsBase);
	auto asm_context = reinterpret_cast<AssemblyCpuContext *>(msr);
	return static_cast<CpuContext *>(asm_context);
}

void callOnCpuStack(void (*function) ()) {
	assert(!intsAreEnabled());

	CpuContext *cpu_context = getCpuContext();
	
	uintptr_t stack_ptr = (uintptr_t)cpu_context->systemStack.base();
	asm volatile ( "mov %0, %%rsp\n"
			"\tcall *%1\n"
			"\tud2\n" : : "r" (stack_ptr), "r" (function) );
	__builtin_unreachable();
}

extern "C" void syscallStub();

void initializeThisProcessor() {
	auto cpu_context = frigg::construct<CpuContext>(*kernelAlloc);
	cpu_context->systemStack = UniqueKernelStack::make();

	// FIXME: the stateSize should not be CPU specific!
	// move it to a global variable and initialize it in initializeTheSystem() etc.!

	// set up the kernel gs segment
	AssemblyCpuContext *asm_context = cpu_context;
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexGsBase, (uintptr_t)asm_context);

	// setup the gdt
	// note: the tss requires two slots in the gdt
	frigg::arch_x86::makeGdtNullSegment(cpu_context->gdt, 0);
	// the layout of the next two kernel descriptors is forced by the use of sysret
	frigg::arch_x86::makeGdtCode64SystemSegment(cpu_context->gdt, 1);
	frigg::arch_x86::makeGdtFlatData32SystemSegment(cpu_context->gdt, 2);
	// the layout of the next three user-space descriptors is forced by the use of sysret
	frigg::arch_x86::makeGdtNullSegment(cpu_context->gdt, 3);
	frigg::arch_x86::makeGdtFlatData32UserSegment(cpu_context->gdt, 4);
	frigg::arch_x86::makeGdtCode64UserSegment(cpu_context->gdt, 5);
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_context->gdt, 6, nullptr, 0);

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 8 * 8;
	gdtr.pointer = cpu_context->gdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq $0x8\n"
			"\rpushq $.L_reloadCs\n"
			"\rlretq\n"
			".L_reloadCs:" );
	
	// setup a stack for irqs
	size_t irq_stack_size = 0x10000;
	void *irq_stack_base = kernelAlloc->allocate(irq_stack_size);
	
	// setup the kernel tss
	frigg::arch_x86::initializeTss64(&cpu_context->tssTemplate);
	cpu_context->tssTemplate.ist1 = (uintptr_t)irq_stack_base + irq_stack_size;
	
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_context->gdt, 6,
			&cpu_context->tssTemplate, sizeof(frigg::arch_x86::Tss64));
	asm volatile ( "ltr %w0" : : "r" ( 0x30 ) );
	
	// setup the idt
	for(int i = 0; i < 256; i++)
		frigg::arch_x86::makeIdt64NullGate(cpu_context->idt, i);
	setupIdt(cpu_context->idt);

	frigg::arch_x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = cpu_context->idt;
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

