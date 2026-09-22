#include <thor-internal/arch-generic/idle.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

namespace {
constexpr IdleState wfiIdleState{
    .name = "WFI", .exitLatency = 0, .targetResidency = 0, .losesTlb = false, .method = {}
};
}

frg::span<const IdleState> getIdleStates() { return {&wfiIdleState, 1}; }

void idleUntilInterrupt(const IdleMethod &) {
	assert(!intsAreEnabled());
	// Wait for an interrupt with interrupts masked.
	asm volatile("wfi" ::: "memory");
	// Flush the pending interrupt.
	enableInts();
	disableInts();
}

} // namespace thor
