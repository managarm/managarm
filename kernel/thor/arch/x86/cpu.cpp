#include <thor-internal/arch/hpet.hpp>
#include <thor-internal/arch/vmx.hpp>
#include <thor-internal/arch/svm.hpp>
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
	void activateTss(common::x86::Tss64 *tss) {
		common::x86::makeGdtTss64Descriptor(getCpuData()->gdt, kGdtIndexTask,
				tss, sizeof(common::x86::Tss64));
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

static constexpr uint16_t fcwInitializer =
	(1 << 0) |    // IM
	(1 << 1) |    // DM
	(1 << 2) |    // ZM
	(1 << 3) |    // OM
	(1 << 4) |    // UM
	(1 << 5) |    // PM
	(0b11 << 8);  // PC

static constexpr uint32_t mxcsrInitializer = 0b1111110000000;


size_t Executor::determineSimdSize() {
	assert(cpuFeaturesKnown);
	if(getGlobalCpuFeatures()->haveXsave){
		return getGlobalCpuFeatures()->xsaveRegionSize;
	}else{
		return sizeof(FxState);
	}
}

size_t Executor::determineSize() {
	// fxState is offset from General by 0x10 bytes to make it 64byte aligned for xsave
	return sizeof(General) + 0x10 + determineSimdSize();
}

Executor::Executor()
: _pointer{nullptr}, _syscallStack{nullptr}, _tss{nullptr} { }

Executor::Executor(UserContext *context, AbiParameters abi) {
	_pointer = (char *)kernelAlloc->allocate(determineSize());
	memset(_pointer, 0, determineSize());

	// Assert assumptions about xsave
	assert(!((uintptr_t)_pointer & 0x3F));
	assert(!((uintptr_t)this->_fxState() & 0x3F));

	_fxState()->mxcsr |= mxcsrInitializer;
	_fxState()->fcw |= fcwInitializer;

	general()->rip = abi.ip;
	general()->rflags = 0x200;
	general()->rsp = abi.sp;
	general()->cs = kSelClientUserCode;
	general()->ss = kSelClientUserData;

	_tss = &context->tss;
	_syscallStack = context->kernelStack.basePtr();
}

Executor::Executor(FiberContext *context, AbiParameters abi)
: _syscallStack{nullptr}, _tss{nullptr} {
	_pointer = (char *)kernelAlloc->allocate(determineSize());
	memset(_pointer, 0, determineSize());

	// Assert assumptions about xsave
	assert(!((uintptr_t)_pointer & 0x3F));
	assert(!((uintptr_t)this->_fxState() & 0x3F));

	_fxState()->mxcsr |= mxcsrInitializer;
	_fxState()->fcw |= fcwInitializer;

	general()->rip = abi.ip;
	general()->rflags = 0x200;
	general()->rsp = (uintptr_t)context->stack.basePtr();
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
	executor->general()->clientFs = common::x86::rdmsr(common::x86::kMsrIndexFsBase);
	executor->general()->clientGs = common::x86::rdmsr(common::x86::kMsrIndexKernelGsBase);

	if(getGlobalCpuFeatures()->haveXsave){
		common::x86::xsave((uint8_t*)executor->_fxState(), ~0);
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
	executor->general()->clientFs = common::x86::rdmsr(common::x86::kMsrIndexFsBase);
	executor->general()->clientGs = common::x86::rdmsr(common::x86::kMsrIndexKernelGsBase);


	if(getGlobalCpuFeatures()->haveXsave){
		common::x86::xsave((uint8_t*)executor->_fxState(), ~0);
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
	executor->general()->clientFs = common::x86::rdmsr(common::x86::kMsrIndexFsBase);
	executor->general()->clientGs = common::x86::rdmsr(common::x86::kMsrIndexKernelGsBase);

	if(getGlobalCpuFeatures()->haveXsave){
		common::x86::xsave((uint8_t*)executor->_fxState(), ~0);
	}else{
		asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
	}
}

void switchExecutor(smarter::borrowed_ptr<Thread> thread) {
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
	common::x86::wrmsr(common::x86::kMsrIndexFsBase, executor->general()->clientFs);
	common::x86::wrmsr(common::x86::kMsrIndexKernelGsBase, executor->general()->clientGs);

	if(getGlobalCpuFeatures()->haveXsave){
		common::x86::xrstor((uint8_t*)executor->_fxState(), ~0);
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
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);;
}

void scrubStack(IrqImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);;
}

void scrubStack(SyscallImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);;
}

void scrubStack(Executor *executor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(*executor->sp()), cont);
}

// --------------------------------------------------------
// UserContext
// --------------------------------------------------------

void UserContext::deactivate() {
	activateTss(&getCpuData()->tss);
}

UserContext::UserContext()
: kernelStack{UniqueKernelStack::make()} {
	memset(&tss, 0, sizeof(common::x86::Tss64));
	common::x86::initializeTss64(&tss);
	tss.rsp0 = (Word)kernelStack.basePtr();
}

void UserContext::enableIoPort(uintptr_t port) {
	tss.ioBitmap[port / 8] &= ~(1 << (port % 8));
}

void UserContext::migrate(CpuData *cpu_data) {
	assert(!intsAreEnabled());
	tss.ist1 = (Word)cpu_data->irqStack.basePtr();
	tss.ist2 = (Word)cpu_data->dfStack.basePtr();
	tss.ist3 = (Word)cpu_data->nmiStack.basePtr();
}

smarter::borrowed_ptr<Thread> activeExecutor() {
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

PlatformCpuData::PlatformCpuData() {
	// Setup the GDT.
	// Note: the TSS requires two slots in the GDT.
	common::x86::makeGdtNullSegment(gdt, kGdtIndexNull);
	common::x86::makeGdtCode64SystemSegment(gdt, kGdtIndexInitialCode);

	common::x86::makeGdtTss64Descriptor(gdt, kGdtIndexTask, nullptr, 0);
	common::x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemIrqCode);

	common::x86::makeGdtCode64SystemSegment(gdt, kGdtIndexExecutorFaultCode);
	common::x86::makeGdtCode64SystemSegment(gdt, kGdtIndexExecutorSyscallCode);
	common::x86::makeGdtFlatData32SystemSegment(gdt, kGdtIndexExecutorKernelData);
	common::x86::makeGdtNullSegment(gdt, kGdtIndexClientUserCompat);
	common::x86::makeGdtFlatData32UserSegment(gdt, kGdtIndexClientUserData);
	common::x86::makeGdtCode64UserSegment(gdt, kGdtIndexClientUserCode);
	common::x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemIdleCode);
	common::x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemFiberCode);

	common::x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemNmiCode);

	// Setup the per-CPU TSS. This TSS is used by system code.
	memset(&tss, 0, sizeof(common::x86::Tss64));
	common::x86::initializeTss64(&tss);
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

constinit bool cpuFeaturesKnown = false;
constinit CpuFeatures globalCpuFeatures{};

initgraph::Stage *getCpuFeaturesKnownStage() {
	static initgraph::Stage s{&globalInitEngine, "x86.cpu-features-known"};
	return &s;
}

static initgraph::Task enumerateCpuFeaturesTask{&globalInitEngine, "x86.enumerate-cpu-features",
	initgraph::Entails{getCpuFeaturesKnownStage()},
	[] {
		// Enable the XSAVE instruction set and child features
		if(common::x86::cpuid(0x1)[2] & (uint32_t(1) << 26)) {
			debugLogger() << "thor: CPUs support XSAVE" << frg::endlog;
			globalCpuFeatures.haveXsave = true;

			auto xsaveCpuid = common::x86::cpuid(0xD);
			globalCpuFeatures.xsaveRegionSize = xsaveCpuid[2];
		}else{
			debugLogger() << "thor: CPUs do not support XSAVE!" << frg::endlog;
		}

		if(globalCpuFeatures.haveXsave) {
			if(common::x86::cpuid(0x1)[2] & (uint32_t(1) << 28)) {
				debugLogger() << "thor: CPUs support AVX" << frg::endlog;
				globalCpuFeatures.haveAvx = true;
			}else{
				debugLogger() << "thor: CPUs do not support AVX!" << frg::endlog;
			}

			if(common::x86::cpuid(0x07)[1] & (uint32_t(1) << 16)) {
				debugLogger() << "thor: CPUs support AVX-512" << frg::endlog;
				globalCpuFeatures.haveZmm = true;
			}else{
				debugLogger() << "thor: CPUs do not support AVX-512!" << frg::endlog;
			}
		}

		if(common::x86::cpuid(0x80000007)[3] & (1 << 8)) {
			debugLogger() << "thor: CPUs support invariant TSC"
					<< frg::endlog;
			globalCpuFeatures.haveInvariantTsc = true;
		}else{
			debugLogger() << "thor: CPUs do not support invariant TSC!" << frg::endlog;
		}

		if(common::x86::cpuid(0x01)[2] & (1 << 24)) {
			debugLogger() << "thor: CPUs support TSC deadline mode"
					<< frg::endlog;
			globalCpuFeatures.haveTscDeadline = true;
		}else{
			debugLogger() << "thor: CPUs do not support TSC deadline mode!"
					<< frg::endlog;
		}

		auto intelPmLeaf = common::x86::cpuid(0xA)[0];
		if(intelPmLeaf & 0xFF) {
			debugLogger() << "thor: CPUs support Intel performance counters"
					<< frg::endlog;
			globalCpuFeatures.profileFlags |= CpuFeatures::profileIntelSupported;
		}
		auto amdPmLeaf = common::x86::cpuid(0x8000'0001)[2];
		if(amdPmLeaf & (1 << 23)) {
			debugLogger() << "thor: CPUs support AMD performance counters"
					<< frg::endlog;
			globalCpuFeatures.profileFlags |= CpuFeatures::profileAmdSupported;
		}

		// Check that both VMX and EPT are supported.
		bool vmxSupported = [] () -> bool {
			// Test for VMX.
			if(!(common::x86::cpuid(0x1)[2] & (1 << 5)))
				return false;
			// Test for secondary processor-based controls.
			auto procBased = common::x86::rdmsr(0x482);
			if(!((procBased >> 32) & (1 << 31)))
				return false;
			// Test for EPT support and unrestricted guests.
			auto procBased2 = common::x86::rdmsr(0x48B);
			if(!((procBased2 >> 32) & (1 << 1)))
				return false;
			if(!((procBased2 >> 32) & (1 << 7)))
				return false;
			// Test if page walks of length 4 are supported by EPT.
			if(!(common::x86::rdmsr(0x48C) & (1 << 6)))
				return false;
			return true;
		}(); // Immediately invoked.

		bool svmSupported = [] () -> bool {
			auto leaf = common::x86::cpuid(common::x86::kCpuIndexExtendedFeatures);
			if(!(leaf[2] & (1 << 2)))
				return false; // Unsupported
			
			auto vm_cr = common::x86::rdmsr(common::x86::kMsrIndexVmCr);
			if(vm_cr & (1 << 4)) {
				if(leaf[3] & (1 << 2)) {
					debugLogger() << "thor: SVM Locked with Key" << frg::endlog;
					return false;
				} else {
					debugLogger() << "thor: SVM Disabled in BIOS" << frg::endlog;
					return false;
				}
			}

			if(!(leaf[3] & (1 << 0)))
				return false; // Required feature NPT unsupported
			return true;
		}();

		if(vmxSupported) {
			debugLogger() << "thor: CPUs support VMX"
					<< frg::endlog;
			globalCpuFeatures.haveVmx = true;
		}else{
			debugLogger() << "thor: CPUs do not support VMX!" << frg::endlog;
		}

		if(svmSupported) {
			debugLogger() << "thor: CPUs support SVM"
					<< frg::endlog;
			globalCpuFeatures.haveSvm = true;
		}else{
			debugLogger() << "thor: CPUs do not support SVM!" << frg::endlog;
		}

		cpuFeaturesKnown = true;
	}
};

namespace {
	frg::manual_box<frg::vector<CpuData *, KernelAlloc>> allCpuContexts;
}

CpuData *getCpuData(size_t k) {
	return (*allCpuContexts)[k];
}

size_t getCpuCount() {
	return allCpuContexts->size();
}

void doRunOnStack(void (*function) (void *, void *), void *sp, void *argument) {
	assert(!intsAreEnabled());

	cleanKasanShadow(reinterpret_cast<std::byte *>(sp) - UniqueKernelStack::kSize,
			UniqueKernelStack::kSize);
	asm volatile (
			"xor %%rbp, %%rbp\n"
			"mov %%rsp, %%rsi\n"
			"\tmov %2, %%rsp\n"
			"\tcall *%1\n"
			"\tud2"
			:
			: "D" (argument), "r" (function), "r" (sp)
			: "rbp", "rsi", "memory");
}

extern "C" void syscallStub();

frg::manual_box<CpuData> staticBootCpuContext;

// Set up the kernel GS segment.
void setupCpuContext(AssemblyCpuData *context) {
	context->selfPointer = context;
	common::x86::wrmsr(common::x86::kMsrIndexGsBase,
			reinterpret_cast<uint64_t>(context));
}

void setupBootCpuContext() {
	staticBootCpuContext.initialize();
	setupCpuContext(staticBootCpuContext.get());
}

static initgraph::Task initBootProcessorTask{&globalInitEngine, "x86.init-boot-processor",
	initgraph::Requires{getCpuFeaturesKnownStage(),
		getApicDiscoveryStage(),
		// HPET is needed for local APIC timer calibration.
		getHpetInitializedStage()},
	initgraph::Entails{getFibersAvailableStage()},
	[] {
		allCpuContexts.initialize(*kernelAlloc);

		// We need to fill in the boot APIC ID.
		// This cannot be done in setupBootCpuContext() as we need the APIC base first.
		staticBootCpuContext->localApicId = getLocalApicId();
		debugLogger() << "Booting on CPU #" << staticBootCpuContext->localApicId
				<< frg::endlog;

		initializeThisProcessor();
	}
};

void initializeThisProcessor() {
	auto cpuData = getCpuData();

	// TODO: If we want to make bootSecondary() parallel, we have to lock here.
	cpuData->cpuIndex = allCpuContexts->size();
	allCpuContexts->push(cpuData);

	// Allocate per-CPU areas.
	cpuData->irqStack = UniqueKernelStack::make();
	cpuData->dfStack = UniqueKernelStack::make();
	cpuData->nmiStack = UniqueKernelStack::make();
	cpuData->detachedStack = UniqueKernelStack::make();
	cpuData->idleStack = UniqueKernelStack::make();

	// We embed some data at the top of the NMI stack.
	// The NMI handler needs this data to enter a consistent kernel state.
	struct Embedded {
		AssemblyCpuData *expectedGs;
		uint64_t padding;
	} embedded{cpuData, 0};

	cpuData->nmiStack.embed<Embedded>(embedded);

	// Setup our IST after the did the embedding.
	cpuData->tss.ist1 = (uintptr_t)cpuData->irqStack.basePtr();
	cpuData->tss.ist2 = (uintptr_t)cpuData->dfStack.basePtr();
	cpuData->tss.ist3 = (uintptr_t)cpuData->nmiStack.basePtr();

	common::x86::Gdtr gdtr;
	gdtr.limit = 14 * 8;
	gdtr.pointer = cpuData->gdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq %0\n"
			"\rpushq $.L_reloadCs\n"
			"\rlretq\n"
			".L_reloadCs:" : : "i" (kSelInitialCode) );

	// We need a valid TSS in case an NMI or fault happens here.
	activateTss(&cpuData->tss);

	// Setup the IDT.
	for(int i = 0; i < 256; i++)
		common::x86::makeIdt64NullGate(cpuData->idt, i);
	setupIdt(cpuData->idt);

	common::x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = cpuData->idt;
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
//	if(!(common::x86::cpuid(common::x86::kCpuIndexStructuredExtendedFeaturesEnum)[1]
//			& common::x86::kCpuFlagFsGsBase))
//		panicLogger() << "CPU does not support wrfsbase / wrgsbase"
//				<< frg::endlog;

//	uint64_t cr4;
//	asm volatile ( "mov %%cr4, %0" : "=r" (cr4) );
//	cr4 |= 0x10000;
//	asm volatile ( "mov %0, %%cr4" : : "r" (cr4) );

	// Enable the XSAVE instruction set and child features
	if(getGlobalCpuFeatures()->haveXsave) {
		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 18; // Enable XSAVE and x{get, set}bv
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		uint64_t xcr0 = 0;
		xcr0 |= (uint64_t(1) << 0); // Enable saving of x87 feature set
		xcr0 |= (uint64_t(1) << 1); // Enable saving of SSE feature set

		if(getGlobalCpuFeatures()->haveAvx)
			xcr0 |= (uint64_t(1) << 2); // Enable saving of AVX feature set and enable it

		if(getGlobalCpuFeatures()->haveZmm) {
			xcr0 |= (uint64_t(1) << 5); // Enable AVX-512
			xcr0 |= (uint64_t(1) << 6); // Enable management of ZMM{0 -> 15}
			xcr0 |= (uint64_t(1) << 7); // Enable management of ZMM{16 -> 31}
		}

		common::x86::wrxcr(0, xcr0);
	}

	// Enable the SMAP extension.
	if(common::x86::cpuid(0x07)[1] & (uint32_t(1) << 20)) {
		debugLogger() << "thor: CPU supports SMAP" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 21;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		asm volatile ("clac" : : : "memory");

		cpuData->haveSmap = true;
	}else{
		debugLogger() << "thor: CPU does not support SMAP!" << frg::endlog;
	}

	// Enable the SMEP extension.
	if(common::x86::cpuid(0x07)[1] & (uint32_t(1) << 6)) {
		debugLogger() << "thor: CPU supports SMEP" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 20;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));
	}else{
		debugLogger() << "thor: CPU does not support SMEP!" << frg::endlog;
	}

	// Enable the UMIP extension.
	if(common::x86::cpuid(0x07)[2] & (uint32_t(1) << 2)) {
		debugLogger() << "thor: CPU supports UMIP" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 11;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));
	}else{
		debugLogger() << "thor: CPU does not support UMIP!" << frg::endlog;
	}

	// Enable the PCID extension.
	bool pcidBit = common::x86::cpuid(0x01)[2] & (uint32_t(1) << 17);
	bool invpcidBit = common::x86::cpuid(0x07)[1] & (uint32_t(1) << 10);
	if(pcidBit && invpcidBit) {
		debugLogger() << "thor: CPU supports PCIDs" << frg::endlog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 17;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		cpuData->havePcids = true;
	}else if(pcidBit) {
		debugLogger() << "thor: CPU supports PCIDs but no INVPCID;"
				" will not use PCIDs!" << frg::endlog;
	}else{
		debugLogger() << "thor: CPU does not support PCIDs!" << frg::endlog;
	}

	// Enable SVM or VMX if it is supported.
	if(getGlobalCpuFeatures()->haveVmx)
		cpuData->haveVirtualization = thor::vmx::vmxon();

	if(getGlobalCpuFeatures()->haveSvm)
		cpuData->haveVirtualization = thor::svm::init();

	// Setup the syscall interface.
	if((common::x86::cpuid(common::x86::kCpuIndexExtendedFeatures)[3]
			& common::x86::kCpuFlagSyscall) == 0)
		panicLogger() << "CPU does not support the syscall instruction"
				<< frg::endlog;

	uint64_t efer = common::x86::rdmsr(common::x86::kMsrEfer);
	common::x86::wrmsr(common::x86::kMsrEfer,
			efer | common::x86::kMsrSyscallEnable);

	common::x86::wrmsr(common::x86::kMsrLstar, (uintptr_t)&syscallStub);
	// Set user mode rpl bits to work around a qemu bug.
	common::x86::wrmsr(common::x86::kMsrStar, (uint64_t(kSelClientUserCompat) << 48)
			| (uint64_t(kSelExecutorSyscallCode) << 32));
	// Mask interrupt and trap flag.
	common::x86::wrmsr(common::x86::kMsrFmask, 0x300);

	// Setup the per-CPU work queue.
	cpuData->wqFiber = KernelFiber::post([] {
		// Do nothing. Our only purpose is to run the associated work queue.
	});
	cpuData->generalWorkQueue = cpuData->wqFiber->associatedWorkQueue()->selfPtr.lock();
	assert(cpuData->generalWorkQueue);

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
	CpuData *cpuContext;
};

static_assert(sizeof(StatusBlock) == 48, "Bad sizeof(StatusBlock)");

void secondaryMain(StatusBlock *statusBlock) {
	auto cpuContext = statusBlock->cpuContext;

	setupCpuContext(cpuContext);
	initializeThisProcessor();
	__atomic_store_n(&statusBlock->targetStage, 2, __ATOMIC_RELEASE);

	debugLogger() << "Hello world from CPU #" << getLocalApicId() << frg::endlog;

	Scheduler::resume(cpuContext->wqFiber);

	auto scheduler = localScheduler();
	scheduler->update();
	scheduler->forceReschedule();
	scheduler->commitReschedule();
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

	auto context = frg::construct<CpuData>(*kernelAlloc);
	context->localApicId = apic_id;

	// Participate in global TLB invalidation *before* paging is used by the target CPU.
	initializeAsidContext(context);

	// Setup a status block to communicate information to the AP.
	auto statusBlock = reinterpret_cast<StatusBlock *>(reinterpret_cast<char *>(accessor.get())
			+ (kPageSize - sizeof(StatusBlock)));
	debugLogger() << "status block accessed via: " << statusBlock << frg::endlog;

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
		pause();
	}
	debugLogger() << "thor: AP did wake up." << frg::endlog;

	// We only let the AP proceed after all IPIs have been sent.
	// This ensures that the AP does not execute boot code twice (e.g. in case
	// it already wakes up after a single SIPI).
	__atomic_store_n(&statusBlock->initiatorStage, 1, __ATOMIC_RELEASE);

	// Wait until the AP exits the boot code.
	while(__atomic_load_n(&statusBlock->targetStage, __ATOMIC_ACQUIRE) < 2) {
		pause();
	}
	debugLogger() << "thor: AP finished booting." << frg::endlog;
}

Error getEntropyFromCpu(void *buffer, size_t size) {
	using word_type = uint32_t;
	auto p = reinterpret_cast<char *>(buffer);

	if(!(common::x86::cpuid(0x7)[1] & (uint32_t(1) << 18)))
		return Error::noHardwareSupport;

	word_type word;
	auto rdseed = [&] () -> bool {
		// Do a maximal number of tries before we give up (e.g., due to broken firmware).
		for(int k = 0; k < 512; ++k) {
			bool success;
			asm volatile ("rdseed %0" : "=r"(word), "=@ccc"(success));
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
