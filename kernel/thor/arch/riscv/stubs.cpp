#include <riscv/csr.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

// TODO: The following functions should be moved to more appropriate places.

uint64_t getRawTimestampCounter() {
	uint64_t v;
	asm volatile("rdtime %0" : "=r"(v));
	return v;
}

// TODO: Hardwire this to true for now. The generic thor codes needs timer to be available.
bool haveTimer() { return true; }

// TODO: Implement these functions correctly:
bool preemptionIsArmed() { return false; }
void armPreemption(uint64_t nanos) { (void)nanos; }
void disarmPreemption() { unimplementedOnRiscv(); }

} // namespace thor
