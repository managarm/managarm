#include <thor-internal/arch/cpu.hpp>
#include <generic/thor-internal/core.hpp>
#include <frg/manual_box.hpp>

namespace thor {

bool FaultImageAccessor::allowUserPages() { assert(!"Not implemented"); return false; }

void UserContext::deactivate() { assert(!"Not implemented"); }
UserContext::UserContext() { assert(!"Not implemented"); }
void UserContext::migrate(CpuData *cpu_data) { assert(!"Not implemented"); }

FiberContext::FiberContext(UniqueKernelStack stack)
: stack{std::move(stack)} { }

[[noreturn]] void restoreExecutor(Executor *executor) { assert(!"Not implemented"); while(true); }

size_t Executor::determineSize() { assert(!"Not implemented"); return 0; }
Executor::Executor() { assert(!"Not implemented"); }
Executor::Executor(UserContext *context, AbiParameters abi) { assert(!"Not implemented"); }

Executor::Executor(FiberContext *context, AbiParameters abi)
: _syscallStack{nullptr} {
	// TODO: set ip, sp, x0 according to abi and context
	thor::infoLogger() << "Executor::Executor(FiberContext, AbiParameters) is unimplemented" << frg::endlog;
}

Executor::~Executor() { assert(!"Not implemented"); }

void saveExecutor(Executor *executor, FaultImageAccessor accessor) { assert(!"Not implemented"); }
void saveExecutor(Executor *executor, IrqImageAccessor accessor) { assert(!"Not implemented"); }
void saveExecutor(Executor *executor, SyscallImageAccessor accessor) { assert(!"Not implemented"); }

extern "C" void doForkExecutor(Executor *executor, void (*functor)(void *), void *context) { assert(!"Not implemented"); }

void workOnExecutor(Executor *executor) { assert(!"Not implemented"); }

void scrubStack(FaultImageAccessor accessor, Continuation cont) { assert(!"Not implemented"); }
void scrubStack(IrqImageAccessor accessor, Continuation cont) { assert(!"Not implemented"); }
void scrubStack(SyscallImageAccessor accessor, Continuation cont) { assert(!"Not implemented"); }
void scrubStack(Executor *executor, Continuation cont) { assert(!"Not implemented"); }

size_t getStateSize() { assert(!"Not implemented"); return 0; }

void switchExecutor(frigg::UnsafePtr<Thread> executor) { assert(!"Not implemented"); }

frigg::UnsafePtr<Thread> activeExecutor() { assert(!"Not implemented"); return nullptr; }

PlatformCpuData::PlatformCpuData() { }

void enableUserAccess() { assert(!"Not implemented"); }
void disableUserAccess() { assert(!"Not implemented"); }
bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor) { assert(!"Not implemented"); return false; }

bool intsAreAllowed() { assert(!"Not implemented"); return false; }
void allowInts() { assert(!"Not implemented"); }

void doRunDetached(void (*function) (void *, void *), void *argument) { assert(!"Not implemented"); }

void initializeThisProcessor() { assert(!"Not implemented"); }

void bootSecondary(unsigned int apic_id) { assert(!"Not implemented"); }

Error getEntropyFromCpu(void *buffer, size_t size) { return Error::noHardwareSupport; }

void armPreemption(uint64_t nanos) { assert(!"Not implemented"); }
void disarmPreemption() { assert(!"Not implemented"); }

uint64_t getRawTimestampCounter() {
	uint64_t cntpct;
	asm volatile ("mrs %0, cntpct_el0" : "=r"(cntpct));
	return cntpct;
}

int getCpuCount() { assert(!"Not implemented"); return 1; }
CpuData *getCpuData(size_t k) { assert(!"Not implemented"); return nullptr; }



frg::manual_box<CpuData> staticBootCpuContext;

void setupCpuContext(AssemblyCpuData *context) {
	context->selfPointer = context;
	asm volatile("msr tpidr_el1, %0" :: "r"(context));
}

void setupBootCpuContext() {
	staticBootCpuContext.initialize();
	setupCpuContext(staticBootCpuContext.get());
}

} // namespace thor
