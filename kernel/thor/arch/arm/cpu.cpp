#include <thor-internal/arch/cpu.hpp>
#include <generic/thor-internal/core.hpp>
#include <frg/manual_box.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/fiber.hpp>

namespace thor {

extern "C" void saveFpSimdRegisters(FpRegisters *frame);
extern "C" void restoreFpSimdRegisters(FpRegisters *frame);

bool FaultImageAccessor::allowUserPages() {
	return true;
}

void UserContext::deactivate() { }
UserContext::UserContext()
: kernelStack(UniqueKernelStack::make()) { }

void UserContext::migrate(CpuData *cpu_data) {
	assert(!intsAreEnabled());
	cpu_data->exceptionStackPtr = kernelStack.base();
}

FiberContext::FiberContext(UniqueKernelStack stack)
: stack{std::move(stack)} { }

extern "C" [[ noreturn ]] void _restoreExecutorRegisters(void *pointer);

[[noreturn]] void restoreExecutor(Executor *executor) {
	getCpuData()->currentDomain = static_cast<uint64_t>(executor->general()->domain);
	getCpuData()->exceptionStackPtr = executor->_exceptionStack;
	restoreFpSimdRegisters(&executor->general()->fp);
	_restoreExecutorRegisters(executor->general());
}

size_t Executor::determineSize() {
	return sizeof(Frame);
}

Executor::Executor()
: _pointer{nullptr}, _exceptionStack{nullptr} {  }

Executor::Executor(UserContext *context, AbiParameters abi) {
	_pointer = static_cast<char *>(kernelAlloc->allocate(getStateSize()));
	memset(_pointer, 0, getStateSize());

	general()->elr = abi.ip;
	general()->sp = abi.sp;
	general()->spsr = 0;
	general()->domain = Domain::user;

	_exceptionStack = context->kernelStack.base();
}

Executor::Executor(FiberContext *context, AbiParameters abi)
: _exceptionStack{nullptr} {
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
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;

	saveFpSimdRegisters(&executor->general()->fp);
}

void saveExecutor(Executor *executor, IrqImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->domain = accessor._frame()->domain;
	executor->general()->sp = accessor._frame()->sp;
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;

	saveFpSimdRegisters(&executor->general()->fp);
}

void saveExecutor(Executor *executor, SyscallImageAccessor accessor) {
	for (int i = 0; i < 31; i++)
		executor->general()->x[i] = accessor._frame()->x[i];

	executor->general()->elr = accessor._frame()->elr;
	executor->general()->spsr = accessor._frame()->spsr;
	executor->general()->domain = accessor._frame()->domain;
	executor->general()->sp = accessor._frame()->sp;
	executor->general()->tpidr_el0 = accessor._frame()->tpidr_el0;

	saveFpSimdRegisters(&executor->general()->fp);
}

extern "C" void workStub();

void workOnExecutor(Executor *executor) {
	auto sp = reinterpret_cast<uint64_t *>(executor->getExceptionStack());
	auto push = [&] (uint64_t v) {
		sp -= 2;
		memcpy(sp, &v, 8);
	};

	assert(executor->general()->domain == Domain::user);
	assert(getCpuData()->currentDomain != static_cast<uint64_t>(Domain::user));

	push(static_cast<uint64_t>(executor->general()->domain));
	push(executor->general()->sp);
	push(executor->general()->elr);
	push(executor->general()->spsr);

	void *stub = reinterpret_cast<void *>(&workStub);
	executor->general()->domain = Domain::fault;
	executor->general()->elr = reinterpret_cast<uintptr_t>(stub);
	executor->general()->sp = reinterpret_cast<uintptr_t>(sp);
	executor->general()->spsr = 0x3c5;
}

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

PlatformCpuData::PlatformCpuData() {
	for(int i = 0; i < maxAsid; i++)
		asidBindings[i].setupAsid(i);
}

// TODO: support PAN?
void enableUserAccess() { }
void disableUserAccess() { }

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

void doRunDetached(void (*function) (void *, void *), void *argument) {
	assert(!intsAreEnabled());

	CpuData *cpuData = getCpuData();

	uintptr_t stackPtr = (uintptr_t)cpuData->detachedStack.base();
	cleanKasanShadow(reinterpret_cast<void *>(stackPtr - UniqueKernelStack::kSize),
			UniqueKernelStack::kSize);

	asm volatile (
			"\tmov x28, sp\n"
			"\tmov x1, sp\n"
			"\tmov x0, %0\n"
			"\tmov sp, %2\n"
			"\tblr %1\n"
			"\tmov sp, x28\n"
			:
			: "r" (argument), "r" (function), "r" (stackPtr)
			: "x30", "x28", "x1", "x0", "memory");
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

static initgraph::Task initBootProcessorTask{&globalInitEngine, "arm.init-boot-processor",
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		allCpuContexts.initialize(*kernelAlloc);

		infoLogger() << "Booting on CPU #0" << frg::endlog;

		initializeThisProcessor();
	}
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

	cpu_data->cpuIndex = allCpuContexts->size();
	allCpuContexts->push(cpu_data);

	cpu_data->irqStack = UniqueKernelStack::make();
	cpu_data->detachedStack = UniqueKernelStack::make();

	cpu_data->irqStackPtr = cpu_data->irqStack.base();

	cpu_data->wqFiber = KernelFiber::post([] {
		// Do nothing. Our only purpose is to run the associated work queue.
	});
	cpu_data->generalWorkQueue = cpu_data->wqFiber->associatedWorkQueue()->selfPtr.lock();
	assert(cpu_data->generalWorkQueue);
}

} // namespace thor
