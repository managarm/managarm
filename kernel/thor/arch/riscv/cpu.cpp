#include <frg/manual_box.hpp>
#include <generic/thor-internal/cpu-data.hpp>
#include <riscv/sbi.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/ring-buffer.hpp>

namespace thor {

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

void UserContext::deactivate() { unimplementedOnRiscv(); }

void saveExecutor(Executor *executor, FaultImageAccessor accessor) { unimplementedOnRiscv(); }
void saveExecutor(Executor *executor, IrqImageAccessor accessor) { unimplementedOnRiscv(); }
void saveExecutor(Executor *executor, SyscallImageAccessor accessor) { unimplementedOnRiscv(); }
void workOnExecutor(Executor *executor) { unimplementedOnRiscv(); }

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

smarter::borrowed_ptr<Thread> activeExecutor() { unimplementedOnRiscv(); }

Error getEntropyFromCpu(void *buffer, size_t size) { return Error::noHardwareSupport; }

void doRunOnStack(void (*function)(void *, void *), void *sp, void *argument) {
	unimplementedOnRiscv();
}

namespace {

constinit frg::manual_box<CpuData> bootCpuContext;
constinit ReentrantRecordRing bootLogRing;

constinit frg::manual_box<frg::vector<CpuData *, KernelAlloc>> allCpuContexts;

void writeToTp(AssemblyCpuData *context) {
	context->selfPointer = context;
	asm volatile("mv tp, %0" : : "r"(context));
}

void handleException() {
	auto cause = riscv::readCsr<riscv::Csr::scause>();
	auto ip = riscv::readCsr<riscv::Csr::sepc>();
	auto trapValue = riscv::readCsr<riscv::Csr::stval>();
	infoLogger() << "thor: Exception with cause 0x" << frg::hex_fmt{cause} << ", trap value 0x"
	             << frg::hex_fmt{trapValue} << " at IP 0x" << frg::hex_fmt{ip} << frg::endlog;
	while (true)
		;
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
	    auto stvec = reinterpret_cast<uint64_t>(&handleException);
	    riscv::writeCsr<riscv::Csr::stvec>(stvec);
    }
};

} // namespace thor
