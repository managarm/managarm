#include <frigg/arch_x86/atomic_impl.hpp>
#include <thor-internal/arch/vmx.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/physical.hpp>

namespace thor {

namespace {
	constexpr bool disableSmp = false;
}

namespace {
	constinit bool cpuFeaturesKnown = false;

	void activateTss(frigg::arch_x86::Tss64 *tss) {
		frigg::arch_x86::makeGdtTss64Descriptor(getCpuData()->gdt, kGdtIndexTask,
				tss, sizeof(frigg::arch_x86::Tss64));
		asm volatile ("ltr %w0" : : "r"(kSelTask) : "memory");
	}
}

// --------------------------------------------------------
// FaultImageAccessor
// --------------------------------------------------------

bool FaultImageAccessor::allowUserPages() {
	assert(inKernelDomain());
	if(!getCpuData()->haveSmap)
		return true;
	return *rflags() & (uint32_t(1) << 18);
}

// --------------------------------------------------------
// Executor
// --------------------------------------------------------

size_t Executor::determineSize() {
	assert(cpuFeaturesKnown);
	auto *cpuData = getCpuData();

	// fxState is offset from General by 0x10 bytes to make it 64byte aligned for xsave
	if(cpuData->haveXsave){
		return sizeof(General) + 0x10 + cpuData->xsaveRegionSize;
	}else{
		return sizeof(General) + 0x10 + sizeof(FxState);
	}
}

Executor::Executor()
: _pointer{nullptr}, _syscallStack{nullptr}, _tss{nullptr} { }

Executor::Executor(UserContext *context, AbiParameters abi) {
	_pointer = (char *)kernelAlloc->allocate(getStateSize());
	memset(_pointer, 0, getStateSize());

	// Assert assumptions about xsave
	assert(!((uintptr_t)_pointer & 0x3F));
	assert(!((uintptr_t)this->_fxState() & 0x3F));

	_fxState()->mxcsr |= 1 << 7;
	_fxState()->mxcsr |= 1 << 8;
	_fxState()->mxcsr |= 1 << 9;
	_fxState()->mxcsr |= 1 << 10;
	_fxState()->mxcsr |= 1 << 11;
	_fxState()->mxcsr |= 1 << 12;

	_fxState()->fcw |= 1 << 0; // IM
	_fxState()->fcw |= 1 << 1; // DM
	_fxState()->fcw |= 1 << 2; // ZM
	_fxState()->fcw |= 1 << 3; // OM
	_fxState()->fcw |= 1 << 4; // UM
	_fxState()->fcw |= 1 << 5; // PM
	_fxState()->fcw |= 0b11 << 8; // PC

	general()->rip = abi.ip;
	general()->rflags = 0x200;
	general()->rsp = abi.sp;
	general()->cs = kSelClientUserCode;
	general()->ss = kSelClientUserData;

	_tss = &context->tss;
	_syscallStack = context->kernelStack.base();
}

Executor::Executor(FiberContext *context, AbiParameters abi)
: _syscallStack{nullptr}, _tss{nullptr} {
	_pointer = (char *)kernelAlloc->allocate(getStateSize());
	memset(_pointer, 0, getStateSize());

	// Assert assumptions about xsave
	assert(!((uintptr_t)_pointer & 0x3F));
	assert(!((uintptr_t)this->_fxState() & 0x3F));

	_fxState()->mxcsr |= 1 << 7;
	_fxState()->mxcsr |= 1 << 8;
	_fxState()->mxcsr |= 1 << 9;
	_fxState()->mxcsr |= 1 << 10;
	_fxState()->mxcsr |= 1 << 11;
	_fxState()->mxcsr |= 1 << 12;

	_fxState()->fcw |= 1 << 0; // IM
	_fxState()->fcw |= 1 << 1; // DM
	_fxState()->fcw |= 1 << 2; // ZM
	_fxState()->fcw |= 1 << 3; // OM
	_fxState()->fcw |= 1 << 4; // UM
	_fxState()->fcw |= 1 << 5; // PM
	_fxState()->fcw |= 0b11 << 8; // PC

	general()->rip = abi.ip;
	general()->rflags = 0x200;
	general()->rsp = (uintptr_t)context->stack.base();
	general()->rdi = abi.argument;
	general()->cs = kSelSystemFiberCode;
	general()->ss = kSelExecutorKernelData;
}

Executor::~Executor() {
	kernelAlloc->free(_pointer);
}

void saveExecutor(Executor *executor, FaultImageAccessor accessor) {
	executor->general()->rax = accessor._frame()->rax;
	executor->general()->rbx = accessor._frame()->rbx;
	executor->general()->rcx = accessor._frame()->rcx;
	executor->general()->rdx = accessor._frame()->rdx;
	executor->general()->rdi = accessor._frame()->rdi;
	executor->general()->rsi = accessor._frame()->rsi;
	executor->general()->rbp = accessor._frame()->rbp;

	executor->general()->r8 = accessor._frame()->r8;
	executor->general()->r9 = accessor._frame()->r9;
	executor->general()->r10 = accessor._frame()->r10;
	executor->general()->r11 = accessor._frame()->r11;
	executor->general()->r12 = accessor._frame()->r12;
	executor->general()->r13 = accessor._frame()->r13;
	executor->general()->r14 = accessor._frame()->r14;
	executor->general()->r15 = accessor._frame()->r15;

	executor->general()->rip = accessor._frame()->rip;
	executor->general()->cs = accessor._frame()->cs;
	executor->general()->rflags = accessor._frame()->rflags;
	executor->general()->rsp = accessor._frame()->rsp;
	executor->general()->ss = accessor._frame()->ss;
	executor->general()->clientFs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexFsBase);
	executor->general()->clientGs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexKernelGsBase);

	if(getCpuData()->haveXsave){
		frigg::arch_x86::xsave((uint8_t*)executor->_fxState(), ~0);
	} else {
		asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
	}
}

void saveExecutor(Executor *executor, IrqImageAccessor accessor) {
	executor->general()->rax = accessor._frame()->rax;
	executor->general()->rbx = accessor._frame()->rbx;
	executor->general()->rcx = accessor._frame()->rcx;
	executor->general()->rdx = accessor._frame()->rdx;
	executor->general()->rdi = accessor._frame()->rdi;
	executor->general()->rsi = accessor._frame()->rsi;
	executor->general()->rbp = accessor._frame()->rbp;

	executor->general()->r8 = accessor._frame()->r8;
	executor->general()->r9 = accessor._frame()->r9;
	executor->general()->r10 = accessor._frame()->r10;
	executor->general()->r11 = accessor._frame()->r11;
	executor->general()->r12 = accessor._frame()->r12;
	executor->general()->r13 = accessor._frame()->r13;
	executor->general()->r14 = accessor._frame()->r14;
	executor->general()->r15 = accessor._frame()->r15;

	executor->general()->rip = accessor._frame()->rip;
	executor->general()->cs = accessor._frame()->cs;
	executor->general()->rflags = accessor._frame()->rflags;
	executor->general()->rsp = accessor._frame()->rsp;
	executor->general()->ss = accessor._frame()->ss;
	executor->general()->clientFs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexFsBase);
	executor->general()->clientGs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexKernelGsBase);


	if(getCpuData()->haveXsave){
		frigg::arch_x86::xsave((uint8_t*)executor->_fxState(), ~0);
	}else{
		asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
	}
}

void saveExecutor(Executor *executor, SyscallImageAccessor accessor) {
	// Note that rbx, rcx and r11 are used internally by the syscall mechanism.
	executor->general()->rax = accessor._frame()->rax;
	executor->general()->rdx = accessor._frame()->rdx;
	executor->general()->rdi = accessor._frame()->rdi;
	executor->general()->rsi = accessor._frame()->rsi;
	executor->general()->rbp = accessor._frame()->rbp;

	executor->general()->r8 = accessor._frame()->r8;
	executor->general()->r9 = accessor._frame()->r9;
	executor->general()->r10 = accessor._frame()->r10;
	executor->general()->r12 = accessor._frame()->r12;
	executor->general()->r13 = accessor._frame()->r13;
	executor->general()->r14 = accessor._frame()->r14;
	executor->general()->r15 = accessor._frame()->r15;

	// Note that we do not save cs and ss on syscall.
	// We just assume that these registers have their usual values.
	executor->general()->rip = accessor._frame()->rip;
	executor->general()->cs = kSelClientUserCode;
	executor->general()->rflags = accessor._frame()->rflags;
	executor->general()->rsp = accessor._frame()->rsp;
	executor->general()->ss = kSelClientUserData;
	executor->general()->clientFs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexFsBase);
	executor->general()->clientGs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexKernelGsBase);

	if(getCpuData()->haveXsave){
		frigg::arch_x86::xsave((uint8_t*)executor->_fxState(), ~0);
	}else{
		asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
	}
}

void switchExecutor(frigg::UnsafePtr<Thread> thread) {
	assert(!intsAreEnabled());
	getCpuData()->activeExecutor = thread;
}

extern "C" void workStub();

void workOnExecutor(Executor *executor) {
	auto nsp = reinterpret_cast<uint64_t *>(executor->getSyscallStack());

	auto push = [&] (uint64_t v) {
		memcpy(--nsp, &v, sizeof(uint64_t));
	};

	// Build an IRET frame on the syscall stack.
	push(*executor->ss());
	push(*executor->sp());
	push(*executor->rflags());
	push(*executor->cs());
	push(*executor->ip());

	// Point the executor to the work stub.
	void *stub = reinterpret_cast<void *>(&workStub);
	*executor->ip() = reinterpret_cast<uintptr_t>(stub);
	*executor->cs() = kSelExecutorSyscallCode;
	*executor->rflags() &= ~uint64_t(0x200); // Disable IRQs.
	*executor->sp() = reinterpret_cast<uintptr_t>(nsp);
	*executor->ss() = 0;
}

extern "C" [[ noreturn ]] void _restoreExecutorRegisters(void *pointer);

[[ gnu::section(".text.stubs") ]] void restoreExecutor(Executor *executor) {
	if(executor->_tss) {
		activateTss(executor->_tss);
	}else{
		activateTss(&getCpuData()->tss);
	}

	getCpuData()->syscallStack = executor->_syscallStack;

	// TODO: use wr{fs,gs}base if it is available
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, executor->general()->clientFs);
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexKernelGsBase, executor->general()->clientGs);

	if(getCpuData()->haveXsave){
		frigg::arch_x86::xrstor((uint8_t*)executor->_fxState(), ~0);
	}else{
		asm volatile ("fxrstorq %0" : : "m" (*executor->_fxState()));
	}

	uint16_t cs = executor->general()->cs;
	assert(cs == kSelExecutorFaultCode || cs == kSelExecutorSyscallCode
			|| cs == kSelClientUserCode || cs == kSelSystemFiberCode);
	if(cs == kSelClientUserCode)
		asm volatile ( "swapgs" : : : "memory" );

	_restoreExecutorRegisters(executor->general());
}

// --------------------------------------------------------
// Stack scrubbing.
// --------------------------------------------------------

void scrubStack(FaultImageAccessor accessor, Continuation cont) {
	auto top = reinterpret_cast<uintptr_t>(accessor.frameBase());
	auto bottom = reinterpret_cast<uintptr_t>(cont.sp);
	assert(top >= bottom);
	cleanKasanShadow(cont.sp, top - bottom);
}

void scrubStack(IrqImageAccessor accessor, Continuation cont) {
	auto top = reinterpret_cast<uintptr_t>(accessor.frameBase());
	auto bottom = reinterpret_cast<uintptr_t>(cont.sp);
	assert(top >= bottom);
	cleanKasanShadow(cont.sp, top - bottom);
}

void scrubStack(SyscallImageAccessor accessor, Continuation cont) {
	auto top = reinterpret_cast<uintptr_t>(accessor.frameBase());
	auto bottom = reinterpret_cast<uintptr_t>(cont.sp);
	assert(top >= bottom);
	cleanKasanShadow(cont.sp, top - bottom);
}

void scrubStack(Executor *executor, Continuation cont) {
	auto top = reinterpret_cast<uintptr_t>(*executor->sp());
	auto bottom = reinterpret_cast<uintptr_t>(cont.sp);
	assert(top >= bottom);
	cleanKasanShadow(cont.sp, top - bottom);
}

// --------------------------------------------------------
// UserContext
// --------------------------------------------------------

void UserContext::deactivate() {
	activateTss(&getCpuData()->tss);
}

UserContext::UserContext()
: kernelStack{UniqueKernelStack::make()} {
	memset(&tss, 0, sizeof(frigg::arch_x86::Tss64));
	frigg::arch_x86::initializeTss64(&tss);
	tss.rsp0 = (Word)kernelStack.base();
}

void UserContext::enableIoPort(uintptr_t port) {
	tss.ioBitmap[port / 8] &= ~(1 << (port % 8));
}

void UserContext::migrate(CpuData *cpu_data) {
	assert(!intsAreEnabled());
	tss.ist1 = (Word)cpu_data->irqStack.base();
	tss.ist2 = (Word)cpu_data->nmiStack.base();
}

frigg::UnsafePtr<Thread> activeExecutor() {
	return getCpuData()->activeExecutor;
}

// --------------------------------------------------------
// FiberContext
// --------------------------------------------------------

FiberContext::FiberContext(UniqueKernelStack stack)
: stack{std::move(stack)} { }

// --------------------------------------------------------
// PlatformCpuData
// --------------------------------------------------------

PlatformCpuData::PlatformCpuData()
: haveSmap{false}, havePcids{false} {
	for(int i = 0; i < maxPcidCount; i++)
		pcidBindings[i].setupPcid(i);

	// Setup the GDT.
	// Note: the TSS requires two slots in the GDT.
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
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemIdleCode);
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemFiberCode);

	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemNmiCode);

	// Setup the per-CPU TSS. This TSS is used by system code.
	memset(&tss, 0, sizeof(frigg::arch_x86::Tss64));
	frigg::arch_x86::initializeTss64(&tss);
}

void enableUserAccess() {
	if(getCpuData()->haveSmap)
		asm volatile ("stac" : : : "memory");
}
void disableUserAccess() {
	if(getCpuData()->haveSmap)
		asm volatile ("clac" : : : "memory");
}

bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor) {
	if(inHigherHalf(address))
		return false;

	auto uar = getCpuData()->currentUar;
	if(!uar)
		return false;

	auto ip = *accessor.ip();
	if(!(ip >= reinterpret_cast<uintptr_t>(uar->startIp)
			&& ip < reinterpret_cast<uintptr_t>(uar->endIp)))
		return false;

	if(write) {
		if(!(uar->flags & uarWrite))
			return false;
	}else{
		if(!(uar->flags & uarRead))
			return false;
	}

	*accessor.ip() = reinterpret_cast<Word>(uar->faultIp);
	return true;
}

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

namespace {
	frigg::LazyInitializer<frigg::Vector<CpuData *, KernelAlloc>> allCpuContexts;
}

size_t getStateSize() {
	return Executor::determineSize();
}

CpuData *getCpuData(size_t k) {
	return (*allCpuContexts)[k];
}

int getCpuCount() {
	return allCpuContexts->size();
}

void doRunDetached(void (*function) (void *, void *), void *argument) {
	assert(!intsAreEnabled());

	CpuData *cpuData = getCpuData();

	uintptr_t stackPtr = (uintptr_t)cpuData->detachedStack.base();
	cleanKasanShadow(reinterpret_cast<void *>(stackPtr - UniqueKernelStack::kSize),
			UniqueKernelStack::kSize);
	asm volatile (
			"mov %%rsp, %%rbp\n"
			"mov %%rsp, %%rsi\n"
			"\tmov %2, %%rsp\n"
			"\tcall *%1\n"
			"\tmov %%rbp, %%rsp"
			:
			: "D" (argument), "r" (function), "r" (stackPtr)
			: "rbp", "rsi", "memory");
}

extern "C" void syscallStub();

frigg::LazyInitializer<CpuData> staticBootCpuContext;

// Set up the kernel GS segment.
void setupCpuContext(AssemblyCpuData *context) {
	context->selfPointer = context;
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexGsBase,
			reinterpret_cast<uint64_t>(context));
}

void setupBootCpuContext() {
	staticBootCpuContext.initialize();
	setupCpuContext(staticBootCpuContext.get());
}

static initgraph::Task initBootProcessorTask{&basicInitEngine, "x86.init-boot-processor",
	initgraph::Requires{getApicDiscoveryStage()},
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		allCpuContexts.initialize(*kernelAlloc);

		// We need to fill in the boot APIC ID.
		// This cannot be done in setupBootCpuContext() as we need the APIC base first.
		staticBootCpuContext->localApicId = getLocalApicId();
		infoLogger() << "Booting on CPU #" << staticBootCpuContext->localApicId
				<< frg::endlog;

		initializeThisProcessor();
	}
};

void initializeThisProcessor() {
	// FIXME: the stateSize should not be CPU specific!
	// move it to a global variable and initialize it in initializeTheSystem() etc.!
	auto cpu_data = getCpuData();

	// TODO: If we want to make bootSecondary() parallel, we have to lock here.
	cpu_data->cpuIndex = allCpuContexts->size();
	allCpuContexts->push(cpu_data);

	// Allocate per-CPU areas.
	cpu_data->irqStack = UniqueKernelStack::make();
	cpu_data->dfStack = UniqueKernelStack::make();
	cpu_data->nmiStack = UniqueKernelStack::make();
	cpu_data->detachedStack = UniqueKernelStack::make();

	// We embed some data at the top of the NMI stack.
	// The NMI handler needs this data to enter a consistent kernel state.
	struct Embedded {
		AssemblyCpuData *expectedGs;
		uint64_t padding;
	} embedded{cpu_data, 0};

	cpu_data->nmiStack.embed<Embedded>(embedded);

	// Setup our IST after the did the embedding.
	cpu_data->tss.ist1 = (uintptr_t)cpu_data->irqStack.base();
	cpu_data->tss.ist2 = (uintptr_t)cpu_data->dfStack.base();
	cpu_data->tss.ist3 = (uintptr_t)cpu_data->nmiStack.base();

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 14 * 8;
	gdtr.pointer = cpu_data->gdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq %0\n"
			"\rpushq $.L_reloadCs\n"
			"\rlretq\n"
			".L_reloadCs:" : : "i" (kSelInitialCode) );

	// We need a valid TSS in case an NMI or fault happens here.
	activateTss(&cpu_data->tss);

	// setup the idt
	for(int i = 0; i < 256; i++)
		frigg::arch_x86::makeIdt64NullGate(cpu_data->idt, i);
	setupIdt(cpu_data->idt);

	frigg::arch_x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = cpu_data->idt;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) );

	// Enable the global page feature.
	{
		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 7;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));
	}

	// Enable the wr{fs,gs}base instructions.
	// FIXME: does not seem to work under qemu
//	if(!(frigg::arch_x86::cpuid(frigg::arch_x86::kCpuIndexStructuredExtendedFeaturesEnum)[1]
//			& frigg::arch_x86::kCpuFlagFsGsBase))
//		panicLogger() << "CPU does not support wrfsbase / wrgsbase"
//				<< frg::endlog;

//	uint64_t cr4;
//	asm volatile ( "mov %%cr4, %0" : "=r" (cr4) );
//	cr4 |= 0x10000;
//	asm volatile ( "mov %0, %%cr4" : : "r" (cr4) );

	// Enable the XSAVE instruction set and child features
	if(frigg::arch_x86::cpuid(0x1)[2] & (uint32_t(1) << 26)) {
		infoLogger() << "\e[37mthor: CPU supports XSAVE\e[39m" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 18; // Enable XSAVE and x{get, set}bv
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		auto xsave_cpuid = frigg::arch_x86::cpuid(0xD);

		uint64_t xcr0 = 0;
		xcr0 |= (uint64_t(1) << 0); // Enable saving of x87 feature set
		xcr0 |= (uint64_t(1) << 1); // Enable saving of SSE feature set

		if(frigg::arch_x86::cpuid(0x1)[2] & (uint32_t(1) << 28)) {
			infoLogger() << "\e[37mthor: CPU supports AVX\e[39m" << frg::endlog;
			xcr0 |= (uint64_t(1) << 2); // Enable saving of AVX feature set and enable it
		}else{
			infoLogger() << "\e[37mthor: CPU does not support AVX!\e[39m" << frg::endlog;
		}

		if(frigg::arch_x86::cpuid(0x07)[1] & (uint32_t(1) << 16)) {
			infoLogger() << "\e[37mthor: CPU supports AVX-512\e[39m" << frg::endlog;
			xcr0 |= (uint64_t(1) << 5); // Enable AVX-512
			xcr0 |= (uint64_t(1) << 6); // Enable management of ZMM{0 -> 15}
			xcr0 |= (uint64_t(1) << 7); // Enable management of ZMM{16 -> 31}
		}else{
			infoLogger() << "\e[37mthor: CPU does not support AVX-512!\e[39m" << frg::endlog;
		}

		frigg::arch_x86::wrxcr(0, xcr0);

		cpu_data->xsaveRegionSize = xsave_cpuid[2];
		cpu_data->haveXsave = true;
	}else{
		infoLogger() << "\e[37mthor: CPU does not support XSAVE!\e[39m" << frg::endlog;
	}

	// Enable the SMAP extension.
	if(frigg::arch_x86::cpuid(0x07)[1] & (uint32_t(1) << 20)) {
		infoLogger() << "\e[37mthor: CPU supports SMAP\e[39m" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 21;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		asm volatile ("clac" : : : "memory");

		cpu_data->haveSmap = true;
	}else{
		infoLogger() << "\e[37mthor: CPU does not support SMAP!\e[39m" << frg::endlog;
	}

	// Enable the SMEP extension.
	if(frigg::arch_x86::cpuid(0x07)[1] & (uint32_t(1) << 6)) {
		infoLogger() << "\e[37mthor: CPU supports SMEP\e[39m" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 20;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

	}else{
		infoLogger() << "\e[37mthor: CPU does not support SMEP!\e[39m" << frg::endlog;
	}

	// Enable the UMIP extension.
	if(frigg::arch_x86::cpuid(0x07)[2] & (uint32_t(1) << 2)) {
		infoLogger() << "\e[37mthor: CPU supports UMIP\e[39m" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 11;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

	}else{
		infoLogger() << "\e[37mthor: CPU does not support UMIP!\e[39m" << frg::endlog;
	}

	// Enable the PCID extension.
	bool pcid_bit = frigg::arch_x86::cpuid(0x01)[2] & (uint32_t(1) << 17);
	bool invpcid_bit = frigg::arch_x86::cpuid(0x07)[1] & (uint32_t(1) << 10);
	if(pcid_bit && invpcid_bit) {
		infoLogger() << "\e[37mthor: CPU supports PCIDs\e[39m" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 17;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		cpu_data->havePcids = true;
	}else if(pcid_bit) {
		infoLogger() << "\e[37mthor: CPU supports PCIDs but no INVPCID;"
				" will not use PCIDs!\e[39m" << frg::endlog;
	}else{
		infoLogger() << "\e[37mthor: CPU does not support PCIDs!\e[39m" << frg::endlog;
	}

	if(frigg::arch_x86::cpuid(0x01)[2] & (1 << 24)) {
		infoLogger() << "\e[37mthor: CPU supports TSC deadline mode\e[39m"
				<< frg::endlog;
		cpu_data->haveTscDeadline = true;
	}

	auto intelPmLeaf = frigg::arch_x86::cpuid(0xA)[0];
	if(intelPmLeaf & 0xFF) {
		infoLogger() << "\e[37mthor: CPU supports Intel performance counters\e[39m"
				<< frg::endlog;
		cpu_data->profileFlags |= PlatformCpuData::profileIntelSupported;
	}
	auto amdPmLeaf = frigg::arch_x86::cpuid(0x8000'0001)[2];
	if(amdPmLeaf & (1 << 23)) {
		infoLogger() << "\e[37mthor: CPU supports AMD performance counters\e[39m"
				<< frg::endlog;
		cpu_data->profileFlags |= PlatformCpuData::profileAmdSupported;
	}

	//Check that both vmx and ept are supported.
	bool vmxSupported = (frigg::arch_x86::cpuid(0x1)[2] >> 5) & 1 && frigg::arch_x86::rdmsr(0x0000048CU) * (1 << 6);
	if(!vmxSupported) {
		infoLogger() << "vmx: vmx not supported" << frg::endlog;
		cpu_data->haveVirtualization = false;
	} else {
		cpu_data->haveVirtualization = thor::vmx::vmxon();
	}

	// setup the syscall interface
	if((frigg::arch_x86::cpuid(frigg::arch_x86::kCpuIndexExtendedFeatures)[3]
			& frigg::arch_x86::kCpuFlagSyscall) == 0)
		panicLogger() << "CPU does not support the syscall instruction"
				<< frg::endlog;

	uint64_t efer = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrEfer);
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrEfer,
			efer | frigg::arch_x86::kMsrSyscallEnable);

	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrLstar, (uintptr_t)&syscallStub);
	// set user mode rpl bits to work around a qemu bug
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrStar, (uint64_t(kSelClientUserCompat) << 48)
			| (uint64_t(kSelExecutorSyscallCode) << 32));
	// mask interrupt and trap flag
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrFmask, 0x300);

	cpuFeaturesKnown = true;

	initLocalApicPerCpu();
}

// Generated by objcopy.
extern "C" uint8_t _binary_kernel_thor_arch_x86_trampoline_bin_start[];
extern "C" uint8_t _binary_kernel_thor_arch_x86_trampoline_bin_end[];

struct StatusBlock {
	StatusBlock *self; // Pointer to this struct in the higher half.
	unsigned int targetStage;
	unsigned int initiatorStage;
	unsigned int pml4;
	uintptr_t stack;
	void (*main)(StatusBlock *);
	AssemblyCpuData *cpuContext;
};

static_assert(sizeof(StatusBlock) == 48, "Bad sizeof(StatusBlock)");

void secondaryMain(StatusBlock *statusBlock) {
	setupCpuContext(statusBlock->cpuContext);
	initializeThisProcessor();
	__atomic_store_n(&statusBlock->targetStage, 2, __ATOMIC_RELEASE);

	infoLogger() << "Hello world from CPU #" << getLocalApicId() << frg::endlog;
	localScheduler()->update();
	localScheduler()->reschedule();
	localScheduler()->commit();
	localScheduler()->invoke();
}

void bootSecondary(unsigned int apic_id) {
	if(disableSmp)
		return;

	// TODO: Allocate a page in low physical memory instead of hard-coding it.
	uintptr_t pma = 0x10000;

	// Copy the trampoline code into low physical memory.
	auto image_size = (uintptr_t)_binary_kernel_thor_arch_x86_trampoline_bin_end
			- (uintptr_t)_binary_kernel_thor_arch_x86_trampoline_bin_start;
	assert(image_size <= kPageSize);
	PageAccessor accessor{pma};
	memcpy(accessor.get(), _binary_kernel_thor_arch_x86_trampoline_bin_start, image_size);

	// Allocate a stack for the initialization code.
	constexpr size_t stack_size = 0x10000;
	void *stack_ptr = kernelAlloc->allocate(stack_size);

	auto context = frigg::construct<CpuData>(*kernelAlloc);
	context->localApicId = apic_id;

	// Participate in global TLB invalidation *before* paging is used by the target CPU.
	{
		auto irqLock = frigg::guard(&irqMutex());

		context->globalBinding.bind();
	}

	// Setup a status block to communicate information to the AP.
	auto statusBlock = reinterpret_cast<StatusBlock *>(reinterpret_cast<char *>(accessor.get())
			+ (kPageSize - sizeof(StatusBlock)));
	infoLogger() << "status block accessed via: " << statusBlock << frg::endlog;

	statusBlock->self = statusBlock;
	statusBlock->targetStage = 0;
	statusBlock->initiatorStage = 0;
	statusBlock->pml4 = KernelPageSpace::global().rootTable();
	statusBlock->stack = (uintptr_t)stack_ptr + stack_size;
	statusBlock->main = &secondaryMain;
	statusBlock->cpuContext = context;

	// Send the IPI sequence that starts up the AP.
	// On modern processors INIT lets the processor enter the wait-for-SIPI state.
	// The BIOS is not involved in this process at all.
	infoLogger() << "thor: Booting AP " << apic_id << "." << frg::endlog;
	raiseInitAssertIpi(apic_id);
	KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(10'000'000)); // Wait for 10ms.

	// SIPI causes the processor to resume execution and resets CS:IP.
	// Intel suggets to send two SIPIs (probably for redundancy reasons).
	raiseStartupIpi(apic_id, pma);
	KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(200'000)); // Wait for 200us.
	raiseStartupIpi(apic_id, pma);
	KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(200'000)); // Wait for 200us.

	// Wait until the AP wakes up.
	while(__atomic_load_n(&statusBlock->targetStage, __ATOMIC_ACQUIRE) < 1) {
		frigg::pause();
	}
	infoLogger() << "thor: AP did wake up." << frg::endlog;

	// We only let the AP proceed after all IPIs have been sent.
	// This ensures that the AP does not execute boot code twice (e.g. in case
	// it already wakes up after a single SIPI).
	__atomic_store_n(&statusBlock->initiatorStage, 1, __ATOMIC_RELEASE);

	// Wait until the AP exits the boot code.
	while(__atomic_load_n(&statusBlock->targetStage, __ATOMIC_ACQUIRE) < 2) {
		frigg::pause();
	}
	infoLogger() << "thor: AP finished booting." << frg::endlog;
}

Error getEntropyFromCpu(void *buffer, size_t size) {
	using word_type = uint32_t;
	auto p = reinterpret_cast<char *>(buffer);

	if(!(frigg::arch_x86::cpuid(0x7)[1] & (uint32_t(1) << 18)))
		return Error::noHardwareSupport;

	word_type word;
	auto rdseed = [&] () -> bool {
		// Do a maximal number of tries before we give up (e.g., due to broken firmware).
		for(int k = 0; k < 512; ++k) {
			bool success;
			asm ("rdseed %0" : "=r"(word), "=@ccc"(success));
			if(success)
				return true;
		}
		return false;
	};

	size_t n = 0;

	// Generate all full words.
	size_t size_words = size & ~(sizeof(word_type) - 1);
	while(n < size_words) {
		if(!rdseed())
			return Error::hardwareBroken;
		memcpy(p + n, &word, sizeof(word_type));
		n += sizeof(word_type);
	}

	// Generate the last word.
	if(n < size) {
		assert(size - n < sizeof(word_type));
		if(!rdseed())
			return Error::hardwareBroken;
		memcpy(p + n, &word, size - n);
	}

	return Error::success;
}

} // namespace thor
