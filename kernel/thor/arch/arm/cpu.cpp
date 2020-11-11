#include <thor-internal/arch/cpu.hpp>
#include <generic/thor-internal/core.hpp>
#include <frg/manual_box.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/fiber.hpp>

namespace thor {

bool FaultImageAccessor::allowUserPages() {
	return true;
}

void UserContext::deactivate() { }
UserContext::UserContext() { }

void UserContext::migrate(CpuData *cpu_data) {
	assert(!intsAreEnabled());
}

FiberContext::FiberContext(UniqueKernelStack stack)
: stack{std::move(stack)} { }

extern "C" [[ noreturn ]] void _restoreExecutorRegisters(void *pointer);

[[noreturn]] void restoreExecutor(Executor *executor) {
	getCpuData()->currentDomain = static_cast<uint64_t>(executor->general()->domain);
	_restoreExecutorRegisters(executor->general());
}

// TODO: later on, store the FPU state as well if supported
size_t Executor::determineSize() {
	return sizeof(Frame);
}

Executor::Executor()
: _pointer{nullptr}, _syscallStack{nullptr} {  }

Executor::Executor(UserContext *context, AbiParameters abi) {
	_pointer = static_cast<char *>(kernelAlloc->allocate(getStateSize()));
	memset(_pointer, 0, getStateSize());

	general()->elr = abi.ip;
	general()->sp = abi.sp;
	general()->spsr = 0;
	general()->domain = Domain::user;

	_syscallStack = context->kernelStack.base();
}

Executor::Executor(FiberContext *context, AbiParameters abi)
: _syscallStack{nullptr} {
	_pointer = static_cast<char *>(kernelAlloc->allocate(getStateSize()));
	memset(_pointer, 0, getStateSize());

	general()->elr = abi.ip;
	general()->sp = (uintptr_t)context->stack.base();
	general()->x[0] = abi.argument;
	general()->spsr = 5;
	general()->domain = Domain::fiber;
}

Executor::~Executor() {
	kernelAlloc->free(_pointer);
}

void saveExecutor(Executor *executor, FaultImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->domain = accessor._frame()->domain;
	executor->general()->sp = accessor._frame()->sp;
}

void saveExecutor(Executor *executor, IrqImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->domain = accessor._frame()->domain;
	executor->general()->sp = accessor._frame()->sp;
}

void saveExecutor(Executor *executor, SyscallImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->domain = accessor._frame()->domain;
	executor->general()->sp = accessor._frame()->sp;
}

void workOnExecutor(Executor *executor) { assert(!"Not implemented"); }

void scrubStack(FaultImageAccessor accessor, Continuation cont) {
	auto top = reinterpret_cast<uintptr_t>(accessor.frameBase());
	auto bottom = reinterpret_cast<uintptr_t>(cont.sp);
	assert(top >= bottom);
	cleanKasanShadow(cont.sp, top - bottom);
	// Perform some sanity checking.
	validateKasanClean(reinterpret_cast<void *>(bottom & ~(kPageSize - 1)),
			bottom & (kPageSize - 1));
}

void scrubStack(IrqImageAccessor accessor, Continuation cont) {
	auto top = reinterpret_cast<uintptr_t>(accessor.frameBase());
	auto bottom = reinterpret_cast<uintptr_t>(cont.sp);
	assert(top >= bottom);
	cleanKasanShadow(cont.sp, top - bottom);
	// Perform some sanity checking.
	validateKasanClean(reinterpret_cast<void *>(bottom & ~(kPageSize - 1)),
			bottom & (kPageSize - 1));
}

void scrubStack(SyscallImageAccessor accessor, Continuation cont) {
	auto top = reinterpret_cast<uintptr_t>(accessor.frameBase());
	auto bottom = reinterpret_cast<uintptr_t>(cont.sp);
	assert(top >= bottom);
	cleanKasanShadow(cont.sp, top - bottom);
	// Perform some sanity checking.
	validateKasanClean(reinterpret_cast<void *>(bottom & ~(kPageSize - 1)),
			bottom & (kPageSize - 1));
}

void scrubStack(Executor *executor, Continuation cont) {
	auto top = reinterpret_cast<uintptr_t>(*executor->sp());
	auto bottom = reinterpret_cast<uintptr_t>(cont.sp);
	assert(top >= bottom);
	cleanKasanShadow(cont.sp, top - bottom);
	// Perform some sanity checking.
	validateKasanClean(reinterpret_cast<void *>(bottom & ~(kPageSize - 1)),
			bottom & (kPageSize - 1));
}

size_t getStateSize() {
	return Executor::determineSize();
}

void switchExecutor(smarter::borrowed_ptr<Thread> thread) {
	assert(!intsAreEnabled());
	getCpuData()->activeExecutor = thread;
}

smarter::borrowed_ptr<Thread> activeExecutor() {
	return getCpuData()->activeExecutor;
}

PlatformCpuData::PlatformCpuData() { }

void enableUserAccess() { assert(!"Not implemented"); }
void disableUserAccess() { assert(!"Not implemented"); }
bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor) { assert(!"Not implemented"); return false; }

bool intsAreAllowed() { assert(!"Not implemented"); return false; }
void allowInts() { assert(!"Not implemented"); }

void doRunDetached(void (*function) (void *, void *), void *argument) {
	assert(!intsAreEnabled());

	CpuData *cpuData = getCpuData();

	uintptr_t stackPtr = (uintptr_t)cpuData->detachedStack.base();
	cleanKasanShadow(reinterpret_cast<void *>(stackPtr - UniqueKernelStack::kSize),
			UniqueKernelStack::kSize);

	asm volatile (
			"\tmov x2, sp\n"
			"\tmov x1, sp\n"
			"\tmov x0, %0\n"
			"\tmov sp, %2\n"
			"\tblr %1\n"
			"\tmov sp, x3\n"
			:
			: "r" (argument), "r" (function), "r" (stackPtr)
			: "x2", "x1", "x0", "memory");
}

void bootSecondary(unsigned int apic_id) { assert(!"Not implemented"); }

Error getEntropyFromCpu(void *buffer, size_t size) { return Error::noHardwareSupport; }

namespace {
	frg::manual_box<frg::vector<CpuData *, KernelAlloc>> allCpuContexts;
}

int getCpuCount() {
	return allCpuContexts->size();
}

CpuData *getCpuData(size_t k) {
	return (*allCpuContexts)[k];
}

frg::manual_box<CpuData> staticBootCpuContext;

void setupCpuContext(AssemblyCpuData *context) {
	context->selfPointer = context;
	asm volatile("msr tpidr_el1, %0" :: "r"(context));
}

void setupBootCpuContext() {
	staticBootCpuContext.initialize();
	setupCpuContext(staticBootCpuContext.get());
}

static initgraph::Task initBootProcessorTask{&basicInitEngine, "arm.init-boot-processor",
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		allCpuContexts.initialize(*kernelAlloc);

		infoLogger() << "Booting on CPU #0" << frg::endlog;

		initializeThisProcessor();
	}
};

extern frg::manual_box<frg::vector<KernelFiber *, KernelAlloc>> earlyFibers;

void initializeThisProcessor() {
	auto cpu_data = getCpuData();

	cpu_data->cpuIndex = allCpuContexts->size();
	allCpuContexts->push(cpu_data);

	cpu_data->exceptionStack = UniqueKernelStack::make();
	cpu_data->detachedStack = UniqueKernelStack::make();
	cpu_data->exceptionStackPtr = cpu_data->exceptionStack.base();

	auto wqFiber = KernelFiber::post([=] {
		// Do nothing. Our only purpose is to run the associated work queue.
	});
	cpu_data->generalWorkQueue = wqFiber->associatedWorkQueue()->selfPtr.lock();
	assert(cpu_data->generalWorkQueue);
	earlyFibers->push(wqFiber);
}

} // namespace thor
