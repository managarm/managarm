#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

void restoreExecutor(Executor *executor) { assert(!"Not implemented"); }


// user-access.S:
extern "C" int doCopyFromUser(void *dest, const void *src, size_t size) { assert(!"Not implemented"); }
extern "C" int doCopyToUser(void *dest, const void *src, size_t size) { assert(!"Not implemented"); }

// TBD; probably timer.cpp
uint64_t getRawTimestampCounter() { assert(!"Not implemented"); }
bool haveTimer() { assert(!"Not implemented"); }
bool preemptionIsArmed() { assert(!"Not implemented"); }
void armPreemption(uint64_t nanos) { assert(!"Not implemented"); }
void disarmPreemption() { assert(!"Not implemented"); }


}