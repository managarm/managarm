
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
	auto pointer = (char *)kernelAlloc->allocate(kSize);
	return UniqueKernelStack(pointer + kSize);
}

UniqueKernelStack::~UniqueKernelStack() {
	if(_base)
		kernelAlloc->free(_base - kSize);
}

// --------------------------------------------------------
// UniqueExecutorImage
// --------------------------------------------------------

size_t UniqueExecutorImage::determineSize() {
	return sizeof(General) + sizeof(FxState);
}

UniqueExecutorImage UniqueExecutorImage::make() {
	auto pointer = (char *)kernelAlloc->allocate(getStateSize());
	memset(pointer, 0, getStateSize());
	return UniqueExecutorImage(pointer);
}

UniqueExecutorImage::~UniqueExecutorImage() {
	kernelAlloc->free(_pointer);
}

void saveExecutorFromFault(FaultImageAccessor accessor) {
	UniqueExecutorImage &image = activeExecutor()->image;

	image._general()->rax = accessor._frame()->rax;
	image._general()->rbx = accessor._frame()->rbx;
	image._general()->rcx = accessor._frame()->rcx;
	image._general()->rdx = accessor._frame()->rdx;
	image._general()->rdi = accessor._frame()->rdi;
	image._general()->rsi = accessor._frame()->rsi;
	image._general()->rbp = accessor._frame()->rbp;

	image._general()->r8 = accessor._frame()->r8;
	image._general()->r9 = accessor._frame()->r9;
	image._general()->r10 = accessor._frame()->r10;
	image._general()->r11 = accessor._frame()->r11;
	image._general()->r12 = accessor._frame()->r12;
	image._general()->r13 = accessor._frame()->r13;
	image._general()->r14 = accessor._frame()->r14;
	image._general()->r15 = accessor._frame()->r15;
	
	image._general()->rsp = accessor._frame()->rsp;
	image._general()->rip = accessor._frame()->rip;
	image._general()->rflags = accessor._frame()->rflags;
	image._general()->kernel = 0;
	image._general()->clientFs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexFsBase);
	image._general()->clientGs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexKernelGsBase);
	
	asm volatile ("fxsaveq %0" : : "m" (*image._fxState()));
}

// --------------------------------------------------------
// PlatformExecutor
// --------------------------------------------------------

PlatformExecutor::PlatformExecutor()
: AssemblyExecutor(UniqueExecutorImage::make(), UniqueKernelStack::make()) {
	memset(&tss, 0, sizeof(frigg::arch_x86::Tss64));
	frigg::arch_x86::initializeTss64(&tss);
	tss.rsp0 = (Word)kernelStack.base();
}

void PlatformExecutor::enableIoPort(uintptr_t port) {
	tss.ioBitmap[port / 8] &= ~(1 << (port % 8));
}

void switchExecutor(frigg::UnsafePtr<Thread> executor) {
	assert(!intsAreEnabled());
	
	executor->getAddressSpace()->activate();

	// setup the thread's tss segment
	CpuData *cpu_data = getCpuData();
	executor->tss.ist1 = (Word)cpu_data->irqStack.base();
	
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_data->gdt, kSegTask,
			&executor->tss, sizeof(frigg::arch_x86::Tss64));
	asm volatile ( "ltr %w0" : : "r" (selectorFor(kSegTask, false)) );

	// finally update the active executor register.
	// we do this after setting up the address space and TSS
	// so that these structures are always valid.
	cpu_data->activeExecutor = executor;
}

frigg::UnsafePtr<Thread> activeExecutor() {
	return frigg::staticPtrCast<Thread>(getCpuData()->activeExecutor);
}

// --------------------------------------------------------
// PlatformCpuData
// --------------------------------------------------------

PlatformCpuData::PlatformCpuData() {
	// setup the gdt
	// note: the tss requires two slots in the gdt
	frigg::arch_x86::makeGdtNullSegment(gdt, kSegNull);
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kSegSystemGeneralCode);
	frigg::arch_x86::makeGdtTss64Descriptor(gdt, kSegTask, nullptr, 0);
	
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kSegSystemIrqCode);

	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kSegExecutorKernelCode);
	frigg::arch_x86::makeGdtFlatData32SystemSegment(gdt, kSegExecutorKernelData);
	frigg::arch_x86::makeGdtNullSegment(gdt, kSegExecutorUserCompat);
	frigg::arch_x86::makeGdtFlatData32UserSegment(gdt, kSegExecutorUserData);
	frigg::arch_x86::makeGdtCode64UserSegment(gdt, kSegExecutorUserCode);
}

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

size_t getStateSize() {
	return UniqueExecutorImage::determineSize();
}

CpuData *getCpuData() {
	uint64_t msr = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexGsBase);
	auto cpu_data = reinterpret_cast<AssemblyCpuData *>(msr);
	return static_cast<CpuData *>(cpu_data);
}

void doRunSystemFunction(void (*function) (void *), void *argument) {
	assert(!intsAreEnabled());

	CpuData *cpu_data = getCpuData();
	
	uintptr_t stack_ptr = (uintptr_t)cpu_data->systemStack.base();
	asm volatile ( "mov %2, %%rsp\n"
			"\tcall *%1\n"
			"\tud2\n" : : "D" (argument), "r" (function), "r" (stack_ptr) );
	__builtin_unreachable();
}

extern "C" void syscallStub();

void initializeThisProcessor() {
	auto cpu_data = frigg::construct<CpuData>(*kernelAlloc);
	cpu_data->irqStack = UniqueKernelStack::make();
	cpu_data->systemStack = UniqueKernelStack::make();

	// FIXME: the stateSize should not be CPU specific!
	// move it to a global variable and initialize it in initializeTheSystem() etc.!

	// set up the kernel gs segment
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexGsBase,
			(uintptr_t)static_cast<AssemblyCpuData *>(cpu_data));

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 10 * 8;
	gdtr.pointer = cpu_data->gdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq %0\n"
			"\rpushq $.L_reloadCs\n"
			"\rlretq\n"
			".L_reloadCs:" : : "i" (selectorFor(kSegSystemGeneralCode, false)) );

	// we enter the idle thread before setting up the IDT.
	// this gives us a valid TSS segment in case an NMI or fault happens here.
	switchExecutor(cpu_data->idleThread);
	infoLogger->log() << "we got here" << frigg::EndLog();
	
	// setup the idt
	for(int i = 0; i < 256; i++)
		frigg::arch_x86::makeIdt64NullGate(cpu_data->idt, i);
	setupIdt(cpu_data->idt);

	frigg::arch_x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = cpu_data->idt;
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
	uint64_t user_selector = selectorFor(kSegExecutorUserCompat, true);
	uint64_t supervisor_selector = selectorFor(kSegExecutorKernelCode, false);
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrStar,
			(user_selector << 48) | (supervisor_selector << 32));
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

