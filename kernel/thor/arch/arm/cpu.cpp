#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch-generic/timer.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/ipl.hpp>
#include <frg/manual_box.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/fiber.hpp>
#include <string.h>

namespace thor {

namespace {

uint64_t readCounterFrequency() {
	uint64_t frequency;
	asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
	return frequency;
}

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
	memcpy(buffer + length, token, tokenLength);
	buffer[length + tokenLength] = 0;
}

uint64_t field(uint64_t value, unsigned int shift) {
	return (value >> shift) & 0xF;
}

void addFeature(bool condition, const char *name) {
	if(condition)
		appendCpuInfoToken(globalCpuFeatures.features,
				sizeof(globalCpuFeatures.features), name);
}

} // namespace

constinit CpuFeatures globalCpuFeatures{};

void initializeCpuFeatures() {
	asm volatile("mrs %0, midr_el1" : "=r"(globalCpuFeatures.midr));
	asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(globalCpuFeatures.idAa64pfr0));
	asm volatile("mrs %0, id_aa64pfr1_el1" : "=r"(globalCpuFeatures.idAa64pfr1));
	asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(globalCpuFeatures.idAa64isar0));
	asm volatile("mrs %0, id_aa64isar1_el1" : "=r"(globalCpuFeatures.idAa64isar1));
	globalCpuFeatures.frequencyHz = readCounterFrequency();
	globalCpuFeatures.bogoMips = calibrateBogoMips(globalCpuFeatures.frequencyHz);

	// These are the feature names used by the AArch64 Linux cpuinfo ABI.
	addFeature(field(globalCpuFeatures.idAa64pfr0, 16) != 0xF, "fp");
	addFeature(field(globalCpuFeatures.idAa64pfr0, 20) != 0xF, "asimd");
	// The generic timer is available on every supported AArch64 platform.
	addFeature(true, "evtstrm");
	addFeature(field(globalCpuFeatures.idAa64isar0, 0) >= 1, "aes");
	addFeature(field(globalCpuFeatures.idAa64isar0, 0) >= 2, "pmull");
	addFeature(field(globalCpuFeatures.idAa64isar0, 4) >= 1, "sha1");
	addFeature(field(globalCpuFeatures.idAa64isar0, 8) >= 1, "sha2");
	addFeature(field(globalCpuFeatures.idAa64isar0, 8) >= 2, "sha512");
	addFeature(field(globalCpuFeatures.idAa64isar0, 12) >= 1, "crc32");
	addFeature(field(globalCpuFeatures.idAa64isar0, 16) >= 1, "atomics");
	addFeature(field(globalCpuFeatures.idAa64isar0, 20) >= 1, "asimdrdm");
	addFeature(field(globalCpuFeatures.idAa64isar0, 24) >= 1, "sha3");
	addFeature(field(globalCpuFeatures.idAa64isar0, 28) >= 1, "sm3");
	addFeature(field(globalCpuFeatures.idAa64isar0, 32) >= 1, "sm4");
	addFeature(field(globalCpuFeatures.idAa64isar0, 36) >= 1, "dcpop");
	addFeature(field(globalCpuFeatures.idAa64isar0, 60) >= 1, "rng");
	addFeature(field(globalCpuFeatures.idAa64pfr0, 32) >= 1, "sve");
	addFeature(field(globalCpuFeatures.idAa64pfr0, 48) >= 1, "dit");
	addFeature(field(globalCpuFeatures.idAa64pfr1, 0) >= 1, "bti");
	addFeature(field(globalCpuFeatures.idAa64pfr1, 4) >= 1, "ssbs");
	addFeature(field(globalCpuFeatures.idAa64pfr1, 8) >= 1, "mte");
	addFeature(field(globalCpuFeatures.idAa64isar1, 12) >= 1, "jscvt");
	addFeature(field(globalCpuFeatures.idAa64isar1, 16) >= 1, "fcma");
	addFeature(field(globalCpuFeatures.idAa64isar1, 20) >= 1, "lrcpc");
	addFeature(field(globalCpuFeatures.idAa64isar1, 44) >= 1, "bf16");
	addFeature(field(globalCpuFeatures.idAa64isar1, 52) >= 1, "i8mm");
}

extern "C" void saveFpSimdRegisters(FpRegisters *frame);
extern "C" void restoreFpSimdRegisters(FpRegisters *frame);

bool FaultImageAccessor::allowUserPages() {
	return true;
}

void UserContext::deactivate() { }
UserContext::UserContext()
: kernelStack(UniqueKernelStack::make()) { }

void UserContext::migrate(CpuData *) {
	assert(!intsAreEnabled());
}

FiberContext::FiberContext(UniqueKernelStack stack)
: stack{std::move(stack)} { }

extern "C" [[ noreturn ]] void _restoreExecutorRegisters(void *pointer, void *el1Stack);

[[noreturn]] void restoreExecutor(Executor *executor) {
	getCpuData()->activeExecutor = executor;
	restoreFpSimdRegisters(executor->fp());

	iplLeaveContext(executor->general()->iplState);

	void *el1Stack = nullptr;
	if ((executor->general()->spsr & 0b1111) == 0b0000) {
		el1Stack = executor->_exceptionStack;
		asm volatile ("msr sp_el0, %0" :: "r"(executor->general()->sp) : "memory");
	} else {
		el1Stack = reinterpret_cast<void *>(executor->general()->sp);
	}

	_restoreExecutorRegisters(executor->general(), el1Stack);
}

Executor::Executor()
: _pointer{nullptr}, _exceptionStack{nullptr} {  }

Executor::Executor(UserContext *context) {
	_pointer = static_cast<char *>(kernelAlloc->allocate(determineSize()));
	memset(_pointer, 0, determineSize());

	_exceptionStack = context->kernelStack.basePtr();
}

Executor::Executor(UserContext *context, void (*launch)())
: Executor{context} {
	general()->elr = reinterpret_cast<uintptr_t>(launch);
	general()->sp = reinterpret_cast<uintptr_t>(_exceptionStack);
	general()->spsr = isKernelInEl2() ? 9 : 5;
}

Executor::Executor(UserContext *context, AbiParameters abi)
: Executor{context} {
	general()->elr = abi.ip;
	general()->sp = abi.sp;
	general()->spsr = 0;
}

Executor::Executor(FiberContext *context, AbiParameters abi)
: _exceptionStack{nullptr} {
	_pointer = static_cast<char *>(kernelAlloc->allocate(determineSize()));
	memset(_pointer, 0, determineSize());

	general()->elr = abi.ip;
	general()->sp = (uintptr_t)context->stack.basePtr();
	general()->x[0] = abi.argument;
	general()->spsr = isKernelInEl2() ? 9 : 5;
}

Executor::~Executor() {
	kernelAlloc->free(_pointer);
}

void saveExecutor(Executor *executor, FaultImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->sp = accessor._frame()->sp;
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;
	executor->general()->iplState = accessor._frame()->iplState;

	saveFpSimdRegisters(executor->fp());
}

void saveExecutor(Executor *executor, IrqImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->sp = accessor._frame()->sp;
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;
	executor->general()->iplState = accessor._frame()->iplState;

	saveFpSimdRegisters(executor->fp());
}

void saveExecutor(Executor *executor, SyscallImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->sp = accessor._frame()->sp;
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;
	executor->general()->iplState = accessor._frame()->iplState;

	saveFpSimdRegisters(executor->fp());
}

extern "C" void forkExecutorRegisters(Executor *executor, void (*functor)(void *), void *context);

void doForkExecutor(Executor *executor, void (*functor)(void *), void *context) {
	iplSave(executor->general()->iplState);

	forkExecutorRegisters(executor, functor, context);
}

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

PlatformCpuData::PlatformCpuData() {
}

// TODO: support PAN?
void enableUserAccess() { }
void disableUserAccess() { }

bool iseqStore64(uint64_t *p, uint64_t v) {
	// TODO: This is a shim. A proper implementation is needed for NMIs on ARM.
	std::atomic_ref{*p}.store(v, std::memory_order_relaxed);
	return true;
}

bool iseqCopyWeak(void *dst, const void *src, size_t size) {
	// TODO: This is a shim. A proper implementation is needed for NMIs on ARM.
	memcpy(dst, src, size);
	return true;
}

void doRunOnStack(void (*function) (void *, void *), void *sp, void *argument) {
	assert(!intsAreEnabled());

	cleanKasanShadow(reinterpret_cast<std::byte *>(sp) - UniqueKernelStack::kSize,
			UniqueKernelStack::kSize);

	asm volatile (
			"\tmov x28, sp\n"
			"\tmov x1, sp\n"
			"\tmov x0, %0\n"
			"\tmov sp, %2\n"
			"\tblr %1\n"
			"\tmov sp, x28\n"
			:
			: "r" (argument), "r" (function), "r" (sp)
			: "x30", "x28", "x1", "x0", "memory");
}

Error getEntropyFromCpu(void *, size_t) { return Error::noHardwareSupport; }

void setupCpuContext(AssemblyCpuData *context) {
	asm volatile("msr tpidr_el1, %0" :: "r"(context));
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

static initgraph::Task initBootProcessorTask{&globalInitEngine, "arm.init-boot-processor",
	initgraph::Entails{getBootProcessorReadyStage()},
	[] {
		infoLogger() << "Booting on CPU #0" << frg::endlog;

		if (isKernelInEl2()) {
			infoLogger() << "Booting in EL2" << frg::endlog;
		} else {
			infoLogger() << "Booting in EL1" << frg::endlog;
		}

		initializeThisProcessor();
	}
};

initgraph::Stage *getBootProcessorReadyStage() {
	static initgraph::Stage s{&globalInitEngine, "arm.boot-processor-ready"};
	return &s;
}

initgraph::Edge bootProcessorReadyEdge{
	getBootProcessorReadyStage(),
	getFibersAvailableStage()
};

void initializeThisProcessor() {
	auto cpu_data = getCpuData();

	// Enable FPU
	asm volatile ("msr cpacr_el1, %0" :: "r"(uint64_t(0b11 << 20)));

	// Enable access to cache info register and cache maintenance instructions
	uint64_t sctlr;
	asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));

	sctlr |= (uint64_t(1) << 14);
	sctlr |= (uint64_t(1) << 15);
	sctlr |= (uint64_t(1) << 26);

	asm volatile ("msr sctlr_el1, %0" :: "r"(sctlr));

	uint64_t mpidr;
	asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	cpu_data->affinity = (mpidr & 0xFFFFFF) | (mpidr >> 32 & 0xFF) << 24;

	cpu_data->detachedStack = UniqueKernelStack::make();
	cpu_data->idleStack = UniqueKernelStack::make();

	cpu_data->wqFiber = KernelFiber::post([] {
		// Do nothing. Our only purpose is to run the associated work queue.
	});
	cpu_data->generalWorkQueue = cpu_data->wqFiber->associatedWorkQueue().lock();
	assert(cpu_data->generalWorkQueue);

	cpu_data->cpuInitialized.store(true, std::memory_order_release);
}

} // namespace thor
