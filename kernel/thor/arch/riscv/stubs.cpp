#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

// TODO: The following functions should be moved to more appropriate places.

void restoreExecutor(Executor *executor) { unimplementedOnRiscv(); }

extern "C" int doCopyFromUser(void *dest, const void *src, size_t size) { unimplementedOnRiscv(); }
extern "C" int doCopyToUser(void *dest, const void *src, size_t size) { unimplementedOnRiscv(); }

uint64_t getRawTimestampCounter() { unimplementedOnRiscv(); }

bool haveTimer() { unimplementedOnRiscv(); }

bool preemptionIsArmed() { unimplementedOnRiscv(); }
void armPreemption(uint64_t nanos) { unimplementedOnRiscv(); }
void disarmPreemption() { unimplementedOnRiscv(); }

} // namespace thor
