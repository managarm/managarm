#include <thor-internal/arch/hpet.hpp>
#include <thor-internal/arch/vmx.hpp>
#include <thor-internal/arch/svm.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/arch/pic.hpp>
#include <x86/machine.hpp>
#include <string.h>

namespace thor {

namespace {
	constexpr bool disableSmp = false;

	void appendCpuInfoToken(char *buffer, size_t capacity, const char *token) {
		size_t length = 0;
		while(length < capacity && buffer[length])
			++length;

		size_t tokenLength = 0;
		while(token[tokenLength])
			++tokenLength;
		for(size_t begin = 0; begin < length;) {
			size_t end = begin;
			while(end < length && buffer[end] != ' ')
				++end;
			if(end - begin == tokenLength
					&& !memcmp(buffer + begin, token, tokenLength))
				return;
			begin = end + 1;
		}

		size_t separatorLength = length ? 1 : 0;
		if(length + separatorLength + tokenLength + 1 > capacity)
			return;

		if(separatorLength)
			buffer[length++] = ' ';
		for(size_t i = 0; i < tokenLength; ++i)
			buffer[length++] = token[i];
		buffer[length] = 0;
	}
}

namespace {
	void activateTss(common::x86::Tss64 *tss) {
		common::x86::makeGdtTss64Descriptor(cpuDescriptorTables.get().gdt, kGdtIndexTask,
				tss, sizeof(common::x86::Tss64));
		asm volatile ("ltr %w0" : : "r"(kSelTask) : "memory");
	}
}

// --------------------------------------------------------
// FaultImageAccessor
// --------------------------------------------------------

bool FaultImageAccessor::allowUserPages() {
	assert(!inUserMode());
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

Executor::Executor(UserContext *context) {
	_pointer = (char *)kernelAlloc->allocate(determineSize());
	memset(_pointer, 0, determineSize());

	// Assert assumptions about xsave.
	assert(!((uintptr_t)_pointer & 0x3F));
	assert(!((uintptr_t)this->_fxState() & 0x3F));

	_fxState()->mxcsr |= mxcsrInitializer;
	_fxState()->fcw |= fcwInitializer;

	_tss = &context->tss;
	_syscallStack = context->kernelStack.basePtr();
}

Executor::Executor(UserContext *context, void (*launch)())
: Executor{context} {
	general()->rip = reinterpret_cast<uintptr_t>(launch);
	general()->rflags = 0x202;
	general()->rsp = reinterpret_cast<uintptr_t>(_syscallStack);
	general()->cs = kSelKernelCode;
	general()->ss = kSelKernelData;
}

Executor::Executor(UserContext *context, AbiParameters abi)
: Executor{context} {
	general()->rip = abi.ip;
	general()->rflags = 0x202;
	general()->rsp = abi.sp;
	general()->cs = kSelUserCode;
	general()->ss = kSelUserData;
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
	general()->rflags = 0x202;
	general()->rsp = (uintptr_t)context->stack.basePtr();
	general()->rdi = abi.argument;
	general()->cs = kSelKernelCode;
	general()->ss = kSelKernelData;
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
	executor->general()->iplState = accessor._frame()->iplState;

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
	executor->general()->iplState = accessor._frame()->iplState;

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
	executor->general()->cs = kSelUserCode;
	executor->general()->rflags = accessor._frame()->rflags;
	executor->general()->rsp = accessor._frame()->rsp;
	executor->general()->ss = kSelUserData;
	executor->general()->clientFs = common::x86::rdmsr(common::x86::kMsrIndexFsBase);
	executor->general()->clientGs = common::x86::rdmsr(common::x86::kMsrIndexKernelGsBase);
	executor->general()->iplState = accessor._frame()->iplState;

	if(getGlobalCpuFeatures()->haveXsave){
		common::x86::xsave((uint8_t*)executor->_fxState(), ~0);
	}else{
		asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
	}
}

extern "C" void forkExecutorRegisters(Executor *executor, void (*functor)(void *), void *context);

void doForkExecutor(Executor *executor, void (*functor)(void *), void *context) {
	iplSave(executor->general()->iplState);

	forkExecutorRegisters(executor, functor, context);
}

extern "C" [[ noreturn ]] void _restoreExecutorRegisters(void *pointer);
extern "C" bool thorFredEnabled;

[[ gnu::section(".text.stubs") ]] void restoreExecutor(Executor *executor) {
	if(executor->_tss) {
		activateTss(executor->_tss);
	}else{
		activateTss(&cpuDescriptorTables.get().tss);
	}

	getCpuData()->activeExecutor = executor;
	getCpuData()->syscallStack = executor->_syscallStack;

	// ensure FRED RSP0 is in sync with the syscall stack
	if (thorFredEnabled)
		common::x86::wrmsr(common::x86::kMsrFredRSP0, (uint64_t)executor->_syscallStack);

	// TODO: use wr{fs,gs}base if it is available
	common::x86::wrmsr(common::x86::kMsrIndexFsBase, executor->general()->clientFs);
	common::x86::wrmsr(common::x86::kMsrIndexKernelGsBase, executor->general()->clientGs);

	if(getGlobalCpuFeatures()->haveXsave){
		common::x86::xrstor((uint8_t*)executor->_fxState(), ~0);
	}else{
		asm volatile ("fxrstorq %0" : : "m" (*executor->_fxState()));
	}

	iplLeaveContext(executor->general()->iplState);

	uint16_t cs = executor->general()->cs;
	assert(cs == kSelKernelCode || cs == kSelUserCode);
	if(cs == kSelUserCode && !thorFredEnabled)
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
	activateTss(&cpuDescriptorTables.get().tss);
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
	tss.ist2 = (Word)cpu_data->dfStack.basePtr();
	tss.ist3 = (Word)cpu_data->nmiStack.basePtr();
}

// --------------------------------------------------------
// FiberContext
// --------------------------------------------------------

FiberContext::FiberContext(UniqueKernelStack stack)
: stack{std::move(stack)} { }

// --------------------------------------------------------
// PlatformCpuData
// --------------------------------------------------------

PlatformCpuData::PlatformCpuData() { }

void enableUserAccess() {
	if(getCpuData()->haveSmap)
		asm volatile ("stac" : : : "memory");
}
void disableUserAccess() {
	if(getCpuData()->haveSmap)
		asm volatile ("clac" : : : "memory");
}

THOR_DEFINE_PERCPU(cpuDescriptorTables);

CpuDescriptorTables::CpuDescriptorTables() {
	// Setup the GDT.
	// Note: the TSS requires two slots in the GDT.
	common::x86::makeGdtNullSegment(gdt, kGdtIndexNull);
	common::x86::makeGdtNullSegment(gdt, kGdtIndexPadding);
	common::x86::makeGdtTss64Descriptor(gdt, kGdtIndexTask, nullptr, 0);
	common::x86::makeGdtCode64SystemSegment(gdt, kGdtIndexKernelCode);
	common::x86::makeGdtFlatData32SystemSegment(gdt, kGdtIndexKernelData);
	common::x86::makeGdtNullSegment(gdt, kGdtIndexUserCompat);
	common::x86::makeGdtFlatData32UserSegment(gdt, kGdtIndexUserData);
	common::x86::makeGdtCode64UserSegment(gdt, kGdtIndexUserCode);

	// Setup the per-CPU TSS. This TSS is used by system code.
	memset(&tss, 0, sizeof(common::x86::Tss64));
	common::x86::initializeTss64(&tss);
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
		auto vendor = common::x86::cpuid(0);
		memcpy(&globalCpuFeatures.vendorId[0], &vendor[1], 4);
		memcpy(&globalCpuFeatures.vendorId[4], &vendor[3], 4);
		memcpy(&globalCpuFeatures.vendorId[8], &vendor[2], 4);
		globalCpuFeatures.vendorId[12] = 0;
		globalCpuFeatures.cpuidLevel = vendor[0];
		auto basicFeatures = common::x86::cpuid(common::x86::kCpuIndexFeatures);
		globalCpuFeatures.clflushSize = ((basicFeatures[1] >> 8) & 0xFF) * 8;
		globalCpuFeatures.cacheAlignment = globalCpuFeatures.clflushSize;
		globalCpuFeatures.fpu = basicFeatures[3] & (1u << 0);
		// x87 floating-point exception reporting is part of the architectural FPU.
		globalCpuFeatures.fpuException = globalCpuFeatures.fpu;
		uint64_t cr0;
		asm volatile ("mov %%cr0, %0" : "=r"(cr0));
		globalCpuFeatures.writeProtect = cr0 & (uint64_t(1) << 16);
		auto extendedLevel = common::x86::cpuid(0x8000'0000)[0];
		frg::array<uint32_t, 4> structuredFeatures{};
		frg::array<uint32_t, 4> structuredFeaturesSubleaf1{};
		frg::array<uint32_t, 4> thermalFeatures{};
		if(vendor[0] >= common::x86::kCpuIndexStructuredExtendedFeaturesEnum) {
			structuredFeatures = common::x86::cpuid(
				common::x86::kCpuIndexStructuredExtendedFeaturesEnum);
			if(structuredFeatures[0] >= 1)
				structuredFeaturesSubleaf1 = common::x86::cpuid(
					common::x86::kCpuIndexStructuredExtendedFeaturesEnum, 1);
		}
		if(vendor[0] >= 0x06)
			thermalFeatures = common::x86::cpuid(0x06);
		frg::array<uint32_t, 4> extendedFeatures{};
		frg::array<uint32_t, 4> extendedFeatures7{};
		if(extendedLevel >= 0x8000'0001)
			extendedFeatures = common::x86::cpuid(0x8000'0001);
		if(extendedLevel >= 0x8000'0007)
			extendedFeatures7 = common::x86::cpuid(0x8000'0007);

		if(vendor[0] >= 0x16) {
			auto frequency = common::x86::cpuid(0x16)[0];
			globalCpuFeatures.frequencyHz = static_cast<uint64_t>(frequency) * 1'000'000;
		}

		auto highestExtendedLeaf = common::x86::cpuid(0x8000'0000)[0];
		globalCpuFeatures.physicalAddressBits = 36;
		globalCpuFeatures.virtualAddressBits = 48;
		if(highestExtendedLeaf >= 0x8000'0008) {
			auto addressSizes = common::x86::cpuid(0x8000'0008)[0];
			if(addressSizes & 0xFF)
				globalCpuFeatures.physicalAddressBits = addressSizes & 0xFF;
			if((addressSizes >> 8) & 0xFF)
				globalCpuFeatures.virtualAddressBits = (addressSizes >> 8) & 0xFF;
		}

		if(highestExtendedLeaf >= 0x8000'0004) {
			for(uint32_t leaf = 0; leaf < 3; ++leaf) {
				auto brand = common::x86::cpuid(0x8000'0002 + leaf);
				for(size_t reg = 0; reg < 4; ++reg)
					memcpy(&globalCpuFeatures.modelName[leaf * 16 + reg * 4], &brand[reg], 4);
			}

			size_t begin = 0;
			while(begin < 48 && globalCpuFeatures.modelName[begin] == ' ')
				++begin;

			size_t end = 48;
			while(end > begin && (globalCpuFeatures.modelName[end - 1] == ' '
					|| globalCpuFeatures.modelName[end - 1] == '\0'))
				--end;

			for(size_t i = begin; i < end; ++i)
				globalCpuFeatures.modelName[i - begin] = globalCpuFeatures.modelName[i];
			globalCpuFeatures.modelName[end - begin] = '\0';
		}

		if(!globalCpuFeatures.modelName[0]) {
			constexpr char fallback[] = "Unknown x86 processor";
			memcpy(globalCpuFeatures.modelName, fallback, sizeof(fallback));
		}

		auto signature = basicFeatures[0];
		auto baseFamily = (signature >> 8) & 0xF;
		auto baseModel = (signature >> 4) & 0xF;
		auto extendedFamily = (signature >> 20) & 0xFF;
		auto extendedModel = (signature >> 16) & 0xF;
		globalCpuFeatures.stepping = signature & 0xF;

		globalCpuFeatures.family = baseFamily;
		if(baseFamily == 0xF)
			globalCpuFeatures.family += extendedFamily;

		globalCpuFeatures.model = baseModel;
		if(baseFamily == 0x6 || baseFamily == 0xF)
			globalCpuFeatures.model |= extendedModel << 4;

		bool isIntel = vendor[1] == 0x756E6547 && vendor[3] == 0x49656E69
				&& vendor[2] == 0x6C65746E;
		bool isAmd = vendor[1] == 0x68747541 && vendor[3] == 0x69746E65
				&& vendor[2] == 0x444D4163;
		auto perfmonFeatures = vendor[0] >= 0x0A
				? common::x86::cpuid(0x0A) : frg::array<uint32_t, 4>{};
		auto artFeatures = vendor[0] >= 0x15
				? common::x86::cpuid(0x15) : frg::array<uint32_t, 4>{};
		uint64_t miscEnable = 0;
		if(isIntel && globalCpuFeatures.family >= 6)
			miscEnable = common::x86::rdmsr(common::x86::kMsrMiscEnable);

		auto addFlag = [](bool present, const char *name) {
			if(present)
				appendCpuInfoToken(globalCpuFeatures.flags,
						sizeof(globalCpuFeatures.flags), name);
		};
		auto addBug = [](bool present, const char *name) {
			if(present)
				appendCpuInfoToken(globalCpuFeatures.bugs,
						sizeof(globalCpuFeatures.bugs), name);
		};
		auto basicEdx = basicFeatures[3];
		auto basicEcx = basicFeatures[2];
		auto extendedEdx = extendedFeatures[3];
		auto extendedEcx = extendedFeatures[2];
		auto structuredEbx = structuredFeatures[1];
		auto structuredEcx = structuredFeatures[2];
		auto structuredEdx = structuredFeatures[3];
		auto structuredSubleaf1Eax = structuredFeaturesSubleaf1[0];
		bool isHypervisor = basicEcx & (1u << 31);
		bool vmxAvailable = false;
		if(isIntel && (basicEcx & (1u << 5))) {
			auto featureControl = common::x86::rdmsr(common::x86::kMsrFeatureControl);
			// An unlocked IA32_FEATURE_CONTROL will be enabled by vmxon(),
			// while a locked MSR must explicitly allow VMX outside SMX.
			vmxAvailable = !(featureControl & 1)
					|| (featureControl & (uint64_t(1) << 2));
		}
		bool svmAvailable = false;
		if(isAmd && (extendedEcx & (1u << 2))) {
			auto vmCr = common::x86::rdmsr(common::x86::kMsrIndexVmCr);
			svmAvailable = !(vmCr & (uint64_t(1) << 4));
		}

		// Keep this list in the same spelling used by Linux's x86
		// /proc/cpuinfo. It includes architectural capabilities and the
		// synthetic capabilities that Thor actually enables or can use.
		addFlag(basicEdx & (1u << 0), "fpu");
		addFlag(basicEdx & (1u << 1), "vme");
		addFlag(basicEdx & (1u << 2), "de");
		addFlag(basicEdx & (1u << 3), "pse");
		addFlag(basicEdx & (1u << 4), "tsc");
		addFlag(basicEdx & (1u << 5), "msr");
		addFlag(basicEdx & (1u << 6), "pae");
		addFlag(basicEdx & (1u << 7), "mce");
		addFlag(basicEdx & (1u << 8), "cx8");
		addFlag(basicEdx & (1u << 9), "apic");
		addFlag(basicEdx & (1u << 11), "sep");
		addFlag(basicEdx & (1u << 12), "mtrr");
		addFlag(basicEdx & (1u << 13), "pge");
		addFlag(basicEdx & (1u << 14), "mca");
		addFlag(basicEdx & (1u << 15), "cmov");
		addFlag(basicEdx & (1u << 16), "pat");
		addFlag(basicEdx & (1u << 17), "pse36");
		addFlag(basicEdx & (1u << 19), "clflush");
		addFlag(basicEdx & (1u << 21), "dts");
		addFlag(basicEdx & (1u << 22), "acpi");
		addFlag(basicEdx & (1u << 23), "mmx");
		addFlag(basicEdx & (1u << 24), "fxsr");
		addFlag(basicEdx & (1u << 25), "sse");
		addFlag(basicEdx & (1u << 26), "sse2");
		addFlag(basicEdx & (1u << 27), "ss");
		addFlag(basicEdx & (1u << 28), "ht");
		addFlag(basicEdx & (1u << 29), "tm");
		addFlag(basicEdx & (1u << 31), "pbe");

		addFlag(extendedEdx & (1u << 11), "syscall");
		addFlag(extendedEdx & (1u << 20), "nx");
		addFlag(extendedEdx & (1u << 22), "mmxext");
		addFlag(extendedEdx & (1u << 25), "fxsr_opt");
		addFlag(extendedEdx & (1u << 26), "pdpe1gb");
		addFlag(extendedEdx & (1u << 27), "rdtscp");
		addFlag(extendedEdx & (1u << 29), "lm");
		addFlag(extendedEdx & (1u << 30), "3dnowext");
		addFlag(extendedEdx & (1u << 31), "3dnow");

		addFlag(basicEcx & (1u << 0), "pni");
		addFlag(basicEcx & (1u << 1), "pclmulqdq");
		addFlag(basicEcx & (1u << 2), "dtes64");
		addFlag(basicEcx & (1u << 3), "monitor");
		addFlag(basicEcx & (1u << 4), "ds_cpl");
		// Linux reports these as finalized CPU capabilities. Their CR4/EFER
		// enablement is performed separately and does not change /proc flags.
		addFlag(vmxAvailable, "vmx");
		addFlag(basicEcx & (1u << 6), "smx");
		addFlag(basicEcx & (1u << 7), "est");
		addFlag(basicEcx & (1u << 8), "tm2");
		addFlag(basicEcx & (1u << 9), "ssse3");
		addFlag(basicEcx & (1u << 10), "cid");
		addFlag(basicEcx & (1u << 11), "sdbg");
		addFlag(basicEcx & (1u << 12), "fma");
		addFlag(basicEcx & (1u << 13), "cx16");
		addFlag(basicEcx & (1u << 14), "xtpr");
		addFlag(basicEcx & (1u << 15), "pdcm");
		addFlag(basicEcx & (1u << 17), "pcid");
		addFlag(basicEcx & (1u << 18), "dca");
		addFlag(basicEcx & (1u << 19), "sse4_1");
		addFlag(basicEcx & (1u << 20), "sse4_2");
		addFlag(basicEcx & (1u << 21), "x2apic");
		addFlag(basicEcx & (1u << 22), "movbe");
		addFlag(basicEcx & (1u << 23), "popcnt");
		addFlag(basicEcx & (1u << 24), "tsc_deadline_timer");
		addFlag(basicEcx & (1u << 25), "aes");
		addFlag(basicEcx & (1u << 26), "xsave");
		addFlag(basicEcx & (1u << 28), "avx");
		addFlag(basicEcx & (1u << 29), "f16c");
		addFlag(basicEcx & (1u << 30), "rdrand");
		addFlag(basicEcx & (1u << 31), "hypervisor");

		addFlag(extendedEcx & (1u << 0), "lahf_lm");
		addFlag(extendedEcx & (1u << 5), "abm");
		addFlag(extendedEcx & (1u << 6), "sse4a");
		addFlag(extendedEcx & (1u << 8), "3dnowprefetch");
		addFlag(extendedEcx & (1u << 11), "xop");
		addFlag(extendedEcx & (1u << 16), "fma4");
		addFlag(extendedEcx & (1u << 21), "tbm");
		addFlag(extendedEcx & (1u << 22), "topoext");

		addFlag(extendedLevel >= 0x8000'0007 && (extendedFeatures7[3] & (1u << 8)), "constant_tsc");
		addFlag(extendedLevel >= 0x8000'0007 && (extendedFeatures7[3] & (1u << 8)), "nonstop_tsc");
		addFlag(true, "cpuid");
		addFlag(svmAvailable, "svm");
		addFlag(vendor[0] >= 0xB && common::x86::cpuid(0xB)[1], "xtopology");
		// These are Linux-defined flags whose state depends on CPUID and/or
		// model-specific registers rather than a single architectural bit.
		addFlag(isIntel && vendor[0] >= 0x0A && (perfmonFeatures[0] & 0xFF)
				&& (((perfmonFeatures[0] >> 8) & 0xFF) > 1), "arch_perfmon");
		addFlag(isIntel && (basicEdx & (1u << 21))
				&& !(miscEnable & (uint64_t(1) << 12)), "pebs");
		addFlag(isIntel && (globalCpuFeatures.family > 6
				|| (globalCpuFeatures.family == 6 && globalCpuFeatures.model >= 0xD))
				&& (miscEnable & (uint64_t(1) << 0)), "rep_good");
		addFlag(isIntel && (basicEdx & (1u << 21))
				&& !(miscEnable & (uint64_t(1) << 11)), "bts");
		// NOPL is unconditionally enabled by Linux on x86-64. Thor is an
		// x86-64 kernel as well, so this is an OS capability, not a CPUID bit.
		addFlag(true, "nopl");
		addFlag(isIntel && !isHypervisor
				&& extendedLevel >= 0x8000'0007
				&& (extendedFeatures7[3] & (1u << 8))
				&& (structuredEbx & (1u << 1))
				&& artFeatures[0] >= 1, "art");
		// cpuid_fault requires a safe probe of the optional
		// MSR_PLATFORM_INFO bit, which Thor does not currently provide.
		// pti is Linux's software KPTI capability and is not a CPUID feature.
		addFlag(vendor[0] >= 0xD && (basicEcx & (1u << 26))
				&& (common::x86::cpuid(0xD, 1)[0] & (1u << 0)), "xsaveopt");
		addFlag(vendor[0] >= 0xD && (basicEcx & (1u << 26))
				&& (common::x86::cpuid(0xD, 1)[0] & (1u << 1)), "xsavec");
		addFlag(vendor[0] >= 0xD && (basicEcx & (1u << 26))
				&& (common::x86::cpuid(0xD, 1)[0] & (1u << 2)), "xgetbv1");
		addFlag(vendor[0] >= 0xD && (basicEcx & (1u << 26))
				&& (common::x86::cpuid(0xD, 1)[0] & (1u << 3)), "xsaves");

		addFlag(structuredEbx & (1u << 0), "fsgsbase");
		addFlag(structuredEbx & (1u << 1), "tsc_adjust");
		addFlag(structuredEbx & (1u << 2), "sgx");
		addFlag(structuredEbx & (1u << 3), "bmi1");
		addFlag(structuredEbx & (1u << 4), "hle");
		addFlag(structuredEbx & (1u << 5), "avx2");
		addFlag(structuredEbx & (1u << 7), "smep");
		addFlag(structuredEbx & (1u << 8), "bmi2");
		addFlag(structuredEbx & (1u << 9), "erms");
		addFlag(structuredEbx & (1u << 10), "invpcid");
		addFlag(structuredEbx & (1u << 11), "rtm");
		addFlag(structuredEbx & (1u << 14), "mpx");
		addFlag(structuredEbx & (1u << 16), "avx512f");
		addFlag(structuredEbx & (1u << 17), "avx512dq");
		addFlag(structuredEbx & (1u << 18), "rdseed");
		addFlag(structuredEbx & (1u << 19), "adx");
		addFlag(structuredEbx & (1u << 20), "smap");
		addFlag(structuredEbx & (1u << 21), "avx512ifma");
		addFlag(structuredEbx & (1u << 23), "clflushopt");
		addFlag(structuredEbx & (1u << 24), "clwb");
		addFlag(structuredEbx & (1u << 25), "intel_pt");
		addFlag(structuredEbx & (1u << 29), "sha_ni");
		addFlag(structuredEbx & (1u << 30), "avx512bw");
		addFlag(structuredEbx & (1u << 31), "avx512vl");

		addFlag(structuredEcx & (1u << 1), "avx512_vbmi");
		addFlag(structuredEcx & (1u << 2), "umip");
		addFlag(structuredEcx & (1u << 3), "pku");
		addFlag(structuredEcx & (1u << 4), "ospke");
		addFlag(structuredEcx & (1u << 5), "waitpkg");
		addFlag(structuredEcx & (1u << 6), "avx512_vbmi2");
		addFlag(structuredEcx & (1u << 7), "shstk");
		addFlag(structuredEcx & (1u << 8), "gfni");
		addFlag(structuredEcx & (1u << 9), "vaes");
		addFlag(structuredEcx & (1u << 10), "vpclmulqdq");
		addFlag(structuredEcx & (1u << 11), "avx512_vnni");
		addFlag(structuredEcx & (1u << 22), "rdpid");
		addFlag(structuredEcx & (1u << 24), "bus_lock_detect");
		addFlag(structuredEcx & (1u << 25), "cldemote");
		addFlag(structuredEcx & (1u << 27), "movdiri");
		addFlag(structuredEcx & (1u << 28), "movdir64b");

		addFlag(structuredEdx & (1u << 4), "fsrm");
		addFlag(structuredEdx & (1u << 10), "md_clear");
		addFlag(structuredEdx & (1u << 14), "serialize");
		addFlag(structuredEdx & (1u << 16), "tsxldtrk");
		addFlag(structuredEdx & (1u << 20), "ibt");
		addFlag(structuredEdx & (1u << 22), "amx_bf16");
		addFlag(structuredEdx & (1u << 24), "amx_tile");
		addFlag(structuredEdx & (1u << 25), "amx_int8");
		addFlag(structuredEdx & (1u << 26), "ibrs");
		addFlag(structuredEdx & (1u << 26), "ibpb");
		addFlag(structuredEdx & (1u << 27), "stibp");
		addFlag(structuredEdx & (1u << 28), "flush_l1d");
		addFlag(structuredEdx & (1u << 29), "arch_capabilities");
		addFlag(structuredEdx & (1u << 31), "ssbd");
		addFlag(structuredSubleaf1Eax & (1u << 4), "avx_vnni");
		addFlag(structuredSubleaf1Eax & (1u << 5), "avx512_bf16");

		addFlag(thermalFeatures[0] & (1u << 0), "dtherm");
		addFlag(thermalFeatures[0] & (1u << 1), "ida");
		addFlag(thermalFeatures[0] & (1u << 2), "arat");
		addFlag(thermalFeatures[0] & (1u << 4), "pln");
		addFlag(thermalFeatures[0] & (1u << 6), "pts");
		addFlag(thermalFeatures[0] & (1u << 7), "hwp");
		addFlag(thermalFeatures[0] & (1u << 8), "hwp_notify");
		addFlag(thermalFeatures[0] & (1u << 9), "hwp_act_window");
		addFlag(thermalFeatures[0] & (1u << 10), "hwp_epp");
		addFlag(thermalFeatures[2] & (1u << 0), "aperfmperf");
		addFlag(thermalFeatures[2] & (1u << 3), "epb");

		// Linux derives these names from Intel VMX capability MSRs. The high
		// halves of the control MSRs enumerate controls that may be enabled.
		if(vmxAvailable) {
			auto vmxPinbasedCtls = common::x86::rdmsr(common::x86::kMsrVmxPinbasedCtls);
			auto vmxProcbasedCtls = common::x86::rdmsr(common::x86::kMsrVmxProcbasedCtls);
			auto vmxProcbasedCtls2 = common::x86::rdmsr(common::x86::kMsrVmxProcbasedCtls2);
			auto vmxEptVpidCap = common::x86::rdmsr(common::x86::kMsrVmxEptVpidCap);
			bool virtualTpr = vmxProcbasedCtls & (uint64_t(1) << (32 + 21));
			bool virtApicAccesses = vmxProcbasedCtls2 & (uint64_t(1) << (32 + 0));
			addFlag(vmxPinbasedCtls & (uint64_t(1) << (32 + 5)), "vnmi");
			addFlag(virtualTpr, "tpr_shadow");
			addFlag(virtualTpr && virtApicAccesses, "flexpriority");
			addFlag(vmxProcbasedCtls2 & (uint64_t(1) << (32 + 1)), "ept");
			addFlag(vmxEptVpidCap & (uint64_t(1) << 32), "vpid");
			addFlag(vmxEptVpidCap & (uint64_t(1) << 21), "ept_ad");
		}

		// These are the Intel client parts derived from Skylake that share the
		// relevant speculation errata. Keep this list deliberately explicit:
		// model numbers are not a reliable indication of a vulnerability across
		// vendors or CPU generations.
		bool isIntelSkylakeClient = isIntel && globalCpuFeatures.family == 6
				&& (globalCpuFeatures.model == 0x4E
					|| globalCpuFeatures.model == 0x5E
					|| globalCpuFeatures.model == 0x8E
					|| globalCpuFeatures.model == 0x9E);
		bool isIntelSrbdsModel = isIntelSkylakeClient
				|| (isIntel && globalCpuFeatures.family == 6
					&& (globalCpuFeatures.model == 0xA5
						|| globalCpuFeatures.model == 0xA6));
		bool isIntelCascadeLake = isIntel && globalCpuFeatures.family == 6
				&& globalCpuFeatures.model == 0x55;
		bool isIntelMmioModel = isIntelSrbdsModel
				|| isIntelCascadeLake;
		bool isIntelRetbleedModel = isIntelSkylakeClient;
		bool isIntelGdsModel = isIntel && globalCpuFeatures.family == 6
				&& (globalCpuFeatures.model == 0x55
					|| globalCpuFeatures.model == 0x4E
					|| globalCpuFeatures.model == 0x5E
					|| globalCpuFeatures.model == 0x7E
					|| globalCpuFeatures.model == 0x6A
					|| globalCpuFeatures.model == 0x6C
					|| globalCpuFeatures.model == 0x8C
					|| globalCpuFeatures.model == 0x8D
					|| globalCpuFeatures.model == 0x8E
					|| globalCpuFeatures.model == 0x9E
					|| globalCpuFeatures.model == 0xA5
					|| globalCpuFeatures.model == 0xA6
					|| globalCpuFeatures.model == 0xA7);
		bool isAmdRetbleedFamily = isAmd && globalCpuFeatures.family == 0x17;
		// RFDS affects only these Intel Atom-derived models. RFDS_NO can
		// subsequently clear the vulnerability after a microcode update.
		bool isIntelRfdsModel = isIntel && globalCpuFeatures.family == 6
				&& (globalCpuFeatures.model == 0x5C
					|| globalCpuFeatures.model == 0x5F
					|| globalCpuFeatures.model == 0x7A
					|| globalCpuFeatures.model == 0x86
					|| globalCpuFeatures.model == 0x96
					|| globalCpuFeatures.model == 0x97
					|| globalCpuFeatures.model == 0x9A
					|| globalCpuFeatures.model == 0x9C
					|| globalCpuFeatures.model == 0xB7
					|| globalCpuFeatures.model == 0xBA
					|| globalCpuFeatures.model == 0xBE
					|| globalCpuFeatures.model == 0xBF);

		// CPUID does not provide a complete vulnerability database. Report the
		// common architectural exposures that can be identified from CPUID,
		// IA32_ARCH_CAPABILITIES, and the model tables above. As in Linux,
		// host-only vulnerability classifications are not exposed to guests.
		uint64_t archCapabilities = 0;
		if(isIntel && (structuredEdx & (1u << 29)))
			archCapabilities = common::x86::rdmsr(common::x86::kMsrArchCapabilities);
		constexpr uint64_t kArchCapRdclNo = uint64_t(1) << 0;
		constexpr uint64_t kArchCapIbrsAll = uint64_t(1) << 1;
		constexpr uint64_t kArchCapSsbNo = uint64_t(1) << 4;
		constexpr uint64_t kArchCapMdsNo = uint64_t(1) << 5;
		constexpr uint64_t kArchCapPsChangeMcNo = uint64_t(1) << 6;
		constexpr uint64_t kArchCapTaaNo = uint64_t(1) << 8;
		constexpr uint64_t kArchCapFbsdpNo = uint64_t(1) << 14;
		constexpr uint64_t kArchCapPsdpNo = uint64_t(1) << 15;
		constexpr uint64_t kArchCapGdsNo = uint64_t(1) << 26;
		constexpr uint64_t kArchCapRfdsNo = uint64_t(1) << 27;
		constexpr uint64_t kArchCapItsNo = uint64_t(1) << 62;
		addBug(isIntel && !(archCapabilities & kArchCapRdclNo), "cpu_meltdown");
		addBug(isIntel || isAmd, "spectre_v1");
		addBug(isIntel || isAmd, "spectre_v2");
		addBug((isIntel || isAmd) && !(archCapabilities & kArchCapSsbNo),
				"spec_store_bypass");
		addBug(isIntel && !(archCapabilities & kArchCapRdclNo), "l1tf");
		addBug(isIntel && !(archCapabilities & kArchCapMdsNo), "mds");
		// Linux reports these vulnerabilities for guests as well. VMSCAPE is the
		// exception: it is explicitly host-only because nested hypervisors are
		// expected to isolate guests:
		// https://www.kernel.org/doc/html/latest/admin-guide/hw-vuln/vmscape.html
		// https://github.com/torvalds/linux/blob/a508cec6e5215a3fbc7e73ae86a5c5602187934d/arch/x86/kernel/cpu/common.c#L1559-L1562
		addBug(isIntelSkylakeClient, "swapgs");
		addBug(!(archCapabilities & kArchCapPsChangeMcNo), "itlb_multihit");
		addBug(isIntelSrbdsModel, "srbds");
		addBug(isIntelMmioModel
				&& !(archCapabilities & kArchCapFbsdpNo)
				&& !(archCapabilities & kArchCapPsdpNo), "mmio_stale_data");
		addBug(isIntelRetbleedModel || isAmdRetbleedFamily,
				"retbleed");
		addBug(isIntelGdsModel && (basicEcx & (1u << 28))
				&& !(archCapabilities & kArchCapGdsNo), "gds");
		addBug(isIntelRfdsModel && !(archCapabilities & kArchCapRfdsNo), "rfds");
		addBug(isIntel || isAmd, "spectre_v2_user");
		addBug(((isIntelSkylakeClient && !(archCapabilities & kArchCapIbrsAll))
				|| (isIntelCascadeLake && !(archCapabilities & kArchCapItsNo))
				|| (isAmd && globalCpuFeatures.family >= 0x17
					&& globalCpuFeatures.family <= 0x1A)) && !isHypervisor, "vmscape");
		addBug(isIntel && (structuredEbx & ((1u << 4) | (1u << 11)))
				&& !(archCapabilities & kArchCapTaaNo), "taa");
		if(isIntel) {
			common::x86::wrmsr(common::x86::kMsrPatchLevel, 0);
			common::x86::cpuid(common::x86::kCpuIndexFeatures);
			globalCpuFeatures.microcodeRevision =
					common::x86::rdmsr(common::x86::kMsrPatchLevel) >> 32;
		}else if(isAmd) {
			globalCpuFeatures.microcodeRevision =
					common::x86::rdmsr(common::x86::kMsrPatchLevel);
		}

		if(common::x86::cpuid(common::x86::kCpuIndexStructuredExtendedFeaturesEnum,1)[0] & common::x86::kCpuFred) {
			debugLogger() << "thor: CPUs support FRED" << frg::endlog;
			globalCpuFeatures.haveFred = true;
		}

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
			// Match Linux: a locked IA32_FEATURE_CONTROL MSR must allow
			// VMX outside SMX. An unlocked MSR can be enabled by vmxon().
			auto featureControl = common::x86::rdmsr(common::x86::kMsrFeatureControl);
			if((featureControl & 1) && !(featureControl & (uint64_t(1) << 2)))
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

// Set up the kernel GS segment.
void setupCpuContext(AssemblyCpuData *context) {
	common::x86::wrmsr(common::x86::kMsrIndexGsBase,
			reinterpret_cast<uint64_t>(context));
}

void prepareCpuDataFor(CpuData *context, int cpu) {
	cpuData.initialize(context);
	heapSlabPool.initialize(context);

	context->selfPointer = context;
	context->cpuIndex = cpu;
}

void setupBootCpuContext() {
	for (size_t c = 0; c < getCpuCount(); ++c)
		prepareCpuDataFor(getCpuData(c), c);

	auto context = getCpuData(0);
	setupCpuContext(context);

}

static initgraph::Task initBootProcessorTask{&globalInitEngine, "x86.init-boot-processor",
	initgraph::Requires{getCpuFeaturesKnownStage(),
		getApicDiscoveryStage(),
		// HPET is needed for local APIC timer calibration.
		getHpetInitializedStage()},
	initgraph::Entails{getFibersAvailableStage()},
	[] {
		// We need to fill in the boot APIC ID.
		// This cannot be done in setupBootCpuContext() as we need the APIC base first.
		cpuData.get().localApicId = getLocalApicId();
		debugLogger() << "Booting on CPU #" << cpuData.get().localApicId
				<< frg::endlog;

		initializeThisProcessor();
	}
};

void initializeThisProcessor() {
	auto cpuData = getCpuData();

	// Allocate per-CPU areas.
	cpuData->dfStack = UniqueKernelStack::make();
	cpuData->nmiStack = UniqueKernelStack::make();
	cpuData->detachedStack = UniqueKernelStack::make();
	cpuData->idleStack = UniqueKernelStack::make();

	// if fred is supported / will be enabled do not embed data into the NMI stack
	if(!globalCpuFeatures.haveFred) {
		// We embed some data at the top of the NMI stack.
		// The NMI handler needs this data to enter a consistent kernel state.
		struct Embedded {
			AssemblyCpuData *expectedGs;
			uint64_t padding;
		} embedded{cpuData, 0};

		cpuData->nmiStack.embed<Embedded>(embedded);
	}

	// Setup our IST after the did the embedding.
	auto *tss = &cpuDescriptorTables.get().tss;
	tss->ist2 = (uintptr_t)cpuData->dfStack.basePtr();
	tss->ist3 = (uintptr_t)cpuData->nmiStack.basePtr();

	common::x86::Gdtr gdtr;
	gdtr.limit = 9 * 8;
	gdtr.pointer = cpuDescriptorTables.get().gdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq %0\n"
			"\rpushq $.L_reloadCs\n"
			"\rlretq\n"
			".L_reloadCs:" : : "i" (kSelKernelCode) );

	// We need a valid TSS in case an NMI or fault happens here.
	activateTss(tss);

	// Setup the syscall interface; this needs to be done early
	// in the case of FRED due to it sharing the IA32_STAR MSR
	if((common::x86::cpuid(common::x86::kCpuIndexExtendedFeatures)[3]
		& common::x86::kCpuFlagSyscall) == 0)
		panicLogger() << "CPU does not support the syscall instruction"
		<< frg::endlog;

	uint64_t efer = common::x86::rdmsr(common::x86::kMsrEfer);
	common::x86::wrmsr(common::x86::kMsrEfer,
					   efer | common::x86::kMsrSyscallEnable);

	common::x86::wrmsr(common::x86::kMsrLstar, (uintptr_t)&syscallStub);
	// Set user mode rpl bits to work around a qemu bug.
	common::x86::wrmsr(common::x86::kMsrStar, (uint64_t(kSelUserCompat) << 48)
	| (uint64_t(kSelKernelCode) << 32));
	// Mask interrupt and trap flag.
	common::x86::wrmsr(
		common::x86::kMsrFmask,
		0x100   // TF.
		| 0x200 // IF.
		| 0x400 // DF.
		| 0x40000 // AC.
	);

	// Setup the IDT. (or FRED if supported by the CPU)
	auto *idt = cpuDescriptorTables.get().idt;
	for(int i = 0; i < 256; i++)
		common::x86::makeIdt64NullGate(idt, i);

	if (globalCpuFeatures.haveFred) {
		// ensure SS has the proper segment for erets
		asm volatile ( "movw %w0, %%ss" : : "r" (kSelKernelData));

		// set the FRED RSPx MSRs for fault stacks
		common::x86::wrmsr(common::x86::kMsrFredRSP2, (uintptr_t)cpuData->dfStack.basePtr());
		common::x86::wrmsr(common::x86::kMsrFredRSP3, (uintptr_t)cpuData->nmiStack.basePtr());

		setupFred();
	} else {
		setupIdt(idt);
	}

	common::x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = idt;
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
	if(common::x86::cpuid(0x07)[1] & (uint32_t(1) << 7)) {
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

	// Setup the per-CPU work queue.
	cpuData->wqFiber = KernelFiber::post([] {
		// Do nothing. Our only purpose is to run the associated work queue.
	});
	cpuData->generalWorkQueue = cpuData->wqFiber->associatedWorkQueue().lock();
	assert(cpuData->generalWorkQueue);

	initLocalApicPerCpu();

	cpuData->cpuInitialized.store(true, std::memory_order_release);
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

	LoadBalancer::singleton().setOnline(cpuContext);
	setRcuOnline(cpuContext);
	auto scheduler = &localScheduler.get();
	scheduler->update();
	scheduler->forceReschedule();
	scheduler->commitReschedule();
}

void bootSecondary(unsigned int apic_id, size_t cpuIndex) {
	if(disableSmp)
		return;

	// Run this function with interrupts disabled to prevent other IPIs (e.g., broadcast IPIs for shootdown)
	// from interfering with CPUs that are not fully online yet.
	auto irqLock = frg::guard(&irqMutex());

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

	auto *context = getCpuData(cpuIndex);
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
	pollSleepNano(10'000'000); // Wait for 10ms.

	// De-assert the INIT IPI.
	raiseInitDeassertIpi(apic_id);
	pollSleepNano(200'000); // Wait for 200us.

	// SIPI causes the processor to resume execution and resets CS:IP.
	// Intel suggets to send two SIPIs (probably for redundancy reasons).
	raiseStartupIpi(apic_id, pma);
	pollSleepNano(200'000); // Wait for 200us.
	raiseStartupIpi(apic_id, pma);
	pollSleepNano(200'000); // Wait for 200us.

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
