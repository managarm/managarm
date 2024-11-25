#include <frg/manual_box.hpp>
#include <generic/thor-internal/cpu-data.hpp>
#include <riscv/sbi.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/ring-buffer.hpp>

namespace thor {

bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor) {
	return false;
}

void enableUserAccess() { unimplementedOnRiscv(); }
void disableUserAccess() { unimplementedOnRiscv(); }

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

UserContext::UserContext() : kernelStack(UniqueKernelStack::make()) {}

void UserContext::migrate(CpuData *) {
	assert(!intsAreEnabled());
	// TODO: ARM refreshes a pointer to the exception stack in CpuData here.
}

void UserContext::deactivate() { unimplementedOnRiscv(); }

void saveExecutor(Executor *executor, FaultImageAccessor accessor) { unimplementedOnRiscv(); }
void saveExecutor(Executor *executor, IrqImageAccessor accessor) { unimplementedOnRiscv(); }
void saveExecutor(Executor *executor, SyscallImageAccessor accessor) { unimplementedOnRiscv(); }
void workOnExecutor(Executor *executor) { unimplementedOnRiscv(); }

Executor::Executor(UserContext *context, AbiParameters abi) {
	size_t size = determineSize();
	_pointer = reinterpret_cast<char *>(kernelAlloc->allocate(size));
	memset(_pointer, 0, size);

	general()->ip = abi.ip;
	general()->sp() = abi.sp;
	general()->umode = 1;

	_exceptionStack = context->kernelStack.basePtr();
}

Executor::Executor(FiberContext *context, AbiParameters abi) {
	size_t size = determineSize();
	_pointer = reinterpret_cast<char *>(kernelAlloc->allocate(size));
	memset(_pointer, 0, size);

	general()->ip = abi.ip;
	general()->sp() = (uintptr_t)context->stack.basePtr();
	general()->a(0) = abi.argument;
}

void scrubStack(FaultImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);
	;
}

void scrubStack(IrqImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);
	;
}

void scrubStack(SyscallImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);
	;
}

void scrubStack(Executor *executor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(*executor->sp()), cont);
}

void switchExecutor(smarter::borrowed_ptr<Thread> thread) {
	assert(!intsAreEnabled());
	getCpuData()->activeExecutor = thread;
}

smarter::borrowed_ptr<Thread> activeExecutor() { return getCpuData()->activeExecutor; }

Error getEntropyFromCpu(void *buffer, size_t size) { return Error::noHardwareSupport; }

void doRunOnStack(void (*function)(void *, void *), void *sp, void *argument) {
	assert(!intsAreEnabled());

	cleanKasanShadow(
	    reinterpret_cast<std::byte *>(sp) - UniqueKernelStack::kSize, UniqueKernelStack::kSize
	);
	register uint64_t a0 asm("a0") = reinterpret_cast<uint64_t>(argument);
	// clang-format off
	asm volatile (
		     "mv a1, sp" "\n"
		"\t" "mv sp, %0" "\n"
		"\t" "jalr %1"   "\n"
		"\t" "unimp"
		:
		: "r"(sp), "r" (function), "r"(a0)
		: "a1", "memory"
	);
	// clang-format on
}

namespace {

constinit frg::manual_box<CpuData> bootCpuContext;
constinit ReentrantRecordRing bootLogRing;

constinit frg::manual_box<frg::vector<CpuData *, KernelAlloc>> allCpuContexts;

void writeToTp(AssemblyCpuData *context) {
	context->selfPointer = context;
	asm volatile("mv tp, %0" : : "r"(context));
}

void initializeThisProcessor() {
	auto cpuData = getCpuData();

	auto scratch = reinterpret_cast<uint64_t>(cpuData);
	riscv::writeCsr<riscv::Csr::sscratch>(scratch);

	cpuData->irqStack = UniqueKernelStack::make();
	cpuData->detachedStack = UniqueKernelStack::make();
	cpuData->idleStack = UniqueKernelStack::make();

	cpuData->irqStackPtr = cpuData->irqStack.basePtr();

	// Install the exception handler after stacks are set up.
	auto stvec = reinterpret_cast<uint64_t>(reinterpret_cast<const void *>(thorExceptionEntry));
	assert(!(stvec & 3));
	riscv::writeCsr<riscv::Csr::stvec>(stvec);

	// Setup the per-CPU work queue.
	cpuData->wqFiber = KernelFiber::post([] {
		// Do nothing. Our only purpose is to run the associated work queue.
	});
	cpuData->generalWorkQueue = cpuData->wqFiber->associatedWorkQueue()->selfPtr.lock();
	assert(cpuData->generalWorkQueue);
}

} // namespace

void setupBootCpuContext() {
	bootCpuContext.initialize();

	bootCpuContext->hartId = thorBootInfoPtr->hartId;
	bootCpuContext->localLogRing = &bootLogRing;

	writeToTp(bootCpuContext.get());
}

CpuData *getCpuData(size_t k) { return (*allCpuContexts)[k]; }

size_t getCpuCount() { return allCpuContexts->size(); }

static initgraph::Task probeSbiFeatures{
    &globalInitEngine,
    "riscv.probe-sbi-features",
    initgraph::Entails{getFibersAvailableStage()},
    [] {
	    if (!sbi::base::probeExtension(sbi::eidIpi))
		    panicLogger() << "SBI does not implement IPI extension" << frg::endlog;
    }
};

static initgraph::Task initBootProcessorTask{
    &globalInitEngine,
    "riscv.init-boot-processor",
    initgraph::Entails{getFibersAvailableStage()},
    [] {
	    allCpuContexts.initialize(*kernelAlloc);

	    auto *cpuData = bootCpuContext.get();
	    cpuData->cpuIndex = 0;
	    allCpuContexts->push(cpuData);

	    debugLogger() << "Booting on HART " << cpuData->hartId << frg::endlog;
	    initializeThisProcessor();
    }
};

} // namespace thor
