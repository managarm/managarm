
#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/io_space.hpp>

#include "generic/kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Debugging functions
// --------------------------------------------------------

int x = 0;
int y = 0;

void advanceY() {
	y++;
	x = 0;
	if(y >= 25) {
		if(haveTimer())
			pollSleepNano(100000000);

		PageAccessor accessor{generalWindow, 0xB8000};
		auto base = (volatile char *)accessor.get();
		for(auto i = 0; i < 24; i++) {
			for(auto j = 0; j < 80; j++) {
				base[(80 * i + j) * 2] = base[(80 * (i + 1) + j) * 2];
				base[(80 * i + j) * 2 + 1] = base[(80 * (i + 1) + j) * 2 + 1];
			}
		}
		for(auto j = 0; j < 80; j++) {
			base[(80 * 24 + j) * 2] = ' ';
			base[(80 * 24 + j) * 2 + 1] = 0x0F;
		}
		y = 24;
	}
}

arch::scalar_register<uint8_t> data(0);
arch::scalar_register<uint8_t> baudLow(0);
arch::scalar_register<uint8_t> baudHigh(1);
arch::bit_register<uint8_t> lineControl(3);
arch::bit_register<uint8_t> lineStatus(5);

arch::field<uint8_t, bool> txReady(5, 1);

arch::field<uint8_t, int> dataBits(0, 2);
arch::field<uint8_t, bool> stopBit(2, 1);
arch::field<uint8_t, int> parityBits(3, 3);
arch::field<uint8_t, bool> dlab(7, 1);

void setupDebugging() {
/*	auto base = arch::global_io.subspace(0x3F8);
	
	// set the baud rate.
	base.store(lineControl, dlab(true));
	base.store(baudLow, 0x0C);
	base.store(baudHigh, 0x00);

	// configure: 8 data bits, 1 stop bit, no parity
	base.store(lineControl, dataBits(3) | stopBit(0) | parityBits(0) | dlab(false));*/
}

void BochsSink::print(char c) {
// --------------------------------------------------------
// Serial console
// --------------------------------------------------------

/*	auto base = arch::global_io.subspace(0x3F8);

	if(c == '\n') {
		while(!(base.load(lineStatus) & txReady)) {
			// do nothing until the UART is ready to transmit.
		}
		base.store(data, '\r');
	}

	while(!(base.load(lineStatus) & txReady)) {
		// do nothing until the UART is ready to transmit.
	}
	base.store(data, c);*/

// --------------------------------------------------------
// Bochs/Qemu debugging port
// --------------------------------------------------------

	frigg::arch_x86::ioOutByte(0xE9, c);

// --------------------------------------------------------
// Text-mode video output
// --------------------------------------------------------
/*
	if(c == '\n') {
		advanceY();
	}else{
		PageAccessor accessor{generalWindow, 0xB8000};
		auto base = (volatile char *)accessor.get();

		base[(80 * y + x) * 2] = c;
		base[(80 * y + x) * 2 + 1] = 0x0F;

		x++;
		if(x >= 80) {
			advanceY();
		}
	}
*/
}
void BochsSink::print(const char *str) {
	while(*str != 0)
		print(*str++);
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

void UniqueExecutorImage::initSystemVAbi(Word ip, Word sp, bool supervisor) {
	memset(_pointer, 0, getStateSize());

	general()->rip = ip;
	general()->rflags = 0x200;
	general()->rsp = sp;

	if(supervisor) {
		general()->cs = kSelExecutorSyscallCode;
		general()->ss = kSelExecutorKernelData;
	}else{
		general()->cs = kSelClientUserCode;
		general()->ss = kSelClientUserData;
	}
}

void saveExecutor(FaultImageAccessor accessor) {
	UniqueExecutorImage &image = activeExecutor()->image;

	image.general()->rax = accessor._frame()->rax;
	image.general()->rbx = accessor._frame()->rbx;
	image.general()->rcx = accessor._frame()->rcx;
	image.general()->rdx = accessor._frame()->rdx;
	image.general()->rdi = accessor._frame()->rdi;
	image.general()->rsi = accessor._frame()->rsi;
	image.general()->rbp = accessor._frame()->rbp;

	image.general()->r8 = accessor._frame()->r8;
	image.general()->r9 = accessor._frame()->r9;
	image.general()->r10 = accessor._frame()->r10;
	image.general()->r11 = accessor._frame()->r11;
	image.general()->r12 = accessor._frame()->r12;
	image.general()->r13 = accessor._frame()->r13;
	image.general()->r14 = accessor._frame()->r14;
	image.general()->r15 = accessor._frame()->r15;
	
	image.general()->rip = accessor._frame()->rip;
	image.general()->cs = accessor._frame()->cs;
	image.general()->rflags = accessor._frame()->rflags;
	image.general()->rsp = accessor._frame()->rsp;
	image.general()->ss = accessor._frame()->ss;
	image.general()->clientFs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexFsBase);
	image.general()->clientGs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexKernelGsBase);
	
	asm volatile ("fxsaveq %0" : : "m" (*image._fxState()));
}

void saveExecutor(SyscallImageAccessor accessor) {
	UniqueExecutorImage &image = activeExecutor()->image;

	// Note that rbx, rcx and r11 are used internally by the syscall mechanism.
	image.general()->rax = accessor._frame()->rax;
	image.general()->rdx = accessor._frame()->rdx;
	image.general()->rdi = accessor._frame()->rdi;
	image.general()->rsi = accessor._frame()->rsi;
	image.general()->rbp = accessor._frame()->rbp;

	image.general()->r8 = accessor._frame()->r8;
	image.general()->r9 = accessor._frame()->r9;
	image.general()->r10 = accessor._frame()->r10;
	image.general()->r12 = accessor._frame()->r12;
	image.general()->r13 = accessor._frame()->r13;
	image.general()->r14 = accessor._frame()->r14;
	image.general()->r15 = accessor._frame()->r15;

	// Note that we do not save cs and ss on syscall.
	// We just assume that these registers have their usual values.
	image.general()->rip = accessor._frame()->rip;
	image.general()->cs = kSelClientUserCode;
	image.general()->rflags = accessor._frame()->rflags;
	image.general()->rsp = accessor._frame()->rsp;
	image.general()->ss = kSelClientUserData;
	image.general()->clientFs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexFsBase);
	image.general()->clientGs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexKernelGsBase);
	
	asm volatile ("fxsaveq %0" : : "m" (*image._fxState()));
}

// --------------------------------------------------------
// PlatformContext
// --------------------------------------------------------

PlatformContext::PlatformContext(void *kernel_stack_base) {
	memset(&tss, 0, sizeof(frigg::arch_x86::Tss64));
	frigg::arch_x86::initializeTss64(&tss);
	tss.rsp0 = (Word)kernel_stack_base;
}

void PlatformContext::enableIoPort(uintptr_t port) {
	tss.ioBitmap[port / 8] &= ~(1 << (port % 8));
}

void switchContext(Context *context) {
	assert(!intsAreEnabled());
	CpuData *cpu_data = getCpuData();
	
//	context->getAddressSpace()->activate();

	// setup the thread's tss segment
	context->tss.ist1 = (Word)cpu_data->irqStack.base();
	
	frigg::arch_x86::makeGdtTss64Descriptor(cpu_data->gdt, kGdtIndexTask,
			&context->tss, sizeof(frigg::arch_x86::Tss64));
	asm volatile ( "ltr %w0" : : "r" (kSelTask) );
}

// --------------------------------------------------------
// PlatformExecutor
// --------------------------------------------------------

PlatformExecutor::PlatformExecutor()
: AssemblyExecutor(UniqueExecutorImage::make(), UniqueKernelStack::make()) {
}

frigg::UnsafePtr<Thread> activeExecutor() {
	return frigg::staticPtrCast<Thread>(getCpuData()->activeExecutor);
}

void switchExecutor(frigg::UnsafePtr<Thread> executor) {
	assert(!intsAreEnabled());
	CpuData *cpu_data = getCpuData();
	
	// finally update the active executor register.
	// we do this after setting up the address space and TSS
	// so that these structures are always valid.
	cpu_data->activeExecutor = executor;
}

extern "C" [[ noreturn ]] void _restoreExecutorRegisters(void *pointer);

[[ gnu::section(".text.stubs") ]] void restoreExecutor() {
	UniqueExecutorImage &image = activeExecutor()->image;

	// TODO: use wr{fs,gs}base if it is available
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, image.general()->clientFs);
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexKernelGsBase, image.general()->clientGs);
	
	uint16_t cs = image.general()->cs;
	assert(cs == kSelExecutorFaultCode || cs == kSelExecutorSyscallCode
			|| cs == kSelClientUserCode);
	if(cs == kSelClientUserCode)
		asm volatile ( "swapgs" : : : "memory" );

	_restoreExecutorRegisters(image._pointer);
}

// --------------------------------------------------------
// PlatformCpuData
// --------------------------------------------------------

PlatformCpuData::PlatformCpuData() {
	// setup the gdt
	// note: the tss requires two slots in the gdt
	frigg::arch_x86::makeGdtNullSegment(gdt, kGdtIndexNull);
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexInitialCode);

	frigg::arch_x86::makeGdtTss64Descriptor(gdt, kGdtIndexTask, nullptr, 0);
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemIrqCode);

	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexExecutorFaultCode);
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexExecutorSyscallCode);
	frigg::arch_x86::makeGdtFlatData32SystemSegment(gdt, kGdtIndexExecutorKernelData);
	frigg::arch_x86::makeGdtNullSegment(gdt, kGdtIndexClientUserCompat);
	frigg::arch_x86::makeGdtFlatData32UserSegment(gdt, kGdtIndexClientUserData);
	frigg::arch_x86::makeGdtCode64UserSegment(gdt, kGdtIndexClientUserCode);
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

void doRunDetached(void (*function) (void *), void *argument) {
	assert(!intsAreEnabled());

	CpuData *cpu_data = getCpuData();
	
	uintptr_t stack_ptr = (uintptr_t)cpu_data->detachedStack.base();
	asm volatile ( "mov %%rsp, %%rbp\n"
			"\tmov %2, %%rsp\n"
			"\tcall *%1\n"
			"\tmov %%rbp, %%rsp" : : "D" (argument), "r" (function), "r" (stack_ptr) : "rbp" );
}

extern "C" void syscallStub();

void initializeThisProcessor() {
	auto cpu_data = frigg::construct<CpuData>(*kernelAlloc);
	cpu_data->irqStack = UniqueKernelStack::make();
	cpu_data->detachedStack = UniqueKernelStack::make();

	// FIXME: the stateSize should not be CPU specific!
	// move it to a global variable and initialize it in initializeTheSystem() etc.!

	// set up the kernel gs segment
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexGsBase,
			(uintptr_t)static_cast<AssemblyCpuData *>(cpu_data));

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 11 * 8;
	gdtr.pointer = cpu_data->gdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq %0\n"
			"\rpushq $.L_reloadCs\n"
			"\rlretq\n"
			".L_reloadCs:" : : "i" (kSelInitialCode) );

	// we enter the idle thread before setting up the IDT.
	// this gives us a valid TSS segment in case an NMI or fault happens here.
	switchContext(&cpu_data->context);
	
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
//		frigg::panicLogger() << "CPU does not support wrfsbase / wrgsbase"
//				<< frigg::endLog;
	
//	uint64_t cr4;
//	asm volatile ( "mov %%cr4, %0" : "=r" (cr4) );
//	cr4 |= 0x10000;
//	asm volatile ( "mov %0, %%cr4" : : "r" (cr4) );

	// setup the syscall interface
	if((frigg::arch_x86::cpuid(frigg::arch_x86::kCpuIndexExtendedFeatures)[3]
			& frigg::arch_x86::kCpuFlagSyscall) == 0)
		frigg::panicLogger() << "CPU does not support the syscall instruction"
				<< frigg::endLog;
	
	uint64_t efer = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrEfer);
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrEfer,
			efer | frigg::arch_x86::kMsrSyscallEnable);

	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrLstar, (uintptr_t)&syscallStub);
	// set user mode rpl bits to work around a qemu bug
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrStar, (uint64_t(kSelClientUserCompat) << 48)
			| (uint64_t(kSelExecutorSyscallCode) << 32));
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

	frigg::infoLogger() << "Hello world from CPU #" << getLocalApicId() << frigg::endLog;	
	initializeThisProcessor();

	frigg::infoLogger() << "Start scheduling on AP" << frigg::endLog;
	ScheduleGuard schedule_guard(scheduleLock.get());
	doSchedule(frigg::move(schedule_guard));
}

void bootSecondary(uint32_t secondary_apic_id) {
	// copy the trampoline code into low physical memory
	uintptr_t trampoline_addr = (uintptr_t)trampoline;
	size_t trampoline_size = (uintptr_t)_trampoline_endLma - (uintptr_t)_trampoline_startLma;
	assert((trampoline_addr % 0x1000) == 0);
	assert((trampoline_size % 0x1000) == 0);
	for(size_t progress = 0; progress < trampoline_size; progress += kPageSize) {
		PageAccessor accessor{generalWindow, trampoline_addr + progress};
		memcpy(accessor.get(), _trampoline_startLma + progress, kPageSize);
	}
	
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
	frigg::infoLogger() << "Waiting for AP to wake up" << frigg::endLog;
	while(frigg::volatileRead<uint32_t>(status_ptr) == 0) {
		frigg::pause();
	}
	
	// allow ap code to initialize the processor
	frigg::infoLogger() << "AP is booting" << frigg::endLog;
	frigg::volatileWrite<uint32_t>(status_ptr, 2);
	
	// wait until the secondary processor completed its boot process
	// we can re-use the trampoline area after this completes
	while(!frigg::volatileRead<bool>(&secondaryBootComplete)) {
		frigg::pause();
	}
	frigg::infoLogger() << "AP finished booting" << frigg::endLog;
}

} // namespace thor

