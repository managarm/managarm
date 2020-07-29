#include <thor-internal/arch/cpu.hpp>

namespace thor {

bool FaultImageAccessor::allowUserPages() { return false; }

void UserContext::deactivate() {}
UserContext::UserContext() {}
void UserContext::migrate(CpuData *cpu_data) {}

FiberContext::FiberContext(UniqueKernelStack stack) {}

[[noreturn]] void restoreExecutor(Executor *executor) { while(true); }

size_t Executor::determineSize() { return 0; }
Executor::Executor() {}
Executor::Executor(UserContext *context, AbiParameters abi) {}
Executor::Executor(FiberContext *context, AbiParameters abi) {}
Executor::~Executor() {}

void saveExecutor(Executor *executor, FaultImageAccessor accessor) {}
void saveExecutor(Executor *executor, IrqImageAccessor accessor) {}
void saveExecutor(Executor *executor, SyscallImageAccessor accessor) {}

extern "C" void doForkExecutor(Executor *executor, void (*functor)(void *), void *context) {}

void workOnExecutor(Executor *executor) {}

size_t getStateSize() { return 0; }

void switchExecutor(frigg::UnsafePtr<Thread> executor) {}

frigg::UnsafePtr<Thread> activeExecutor() { return nullptr; }

PlatformCpuData::PlatformCpuData() {}

void enableUserAccess() {}
void disableUserAccess() {}
bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor) { return false; }

bool intsAreAllowed() { return false; }
void allowInts() {}

void doRunDetached(void (*function) (void *), void *argument) {}

void initializeThisProcessor() {}

void bootSecondary(unsigned int apic_id) {}

Error getEntropyFromCpu(void *buffer, size_t size) { return Error::noHardwareSupport; }

void armPreemption(uint64_t nanos) {}
void disarmPreemption() {}
uint64_t getRawTimestampCounter() { return 0; }

int getCpuCount() { return 1; }
CpuData *getCpuData(size_t k) { return nullptr; }

} // namespace thor
