#include <thor-internal/arch-generic/idle.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

namespace {
	constexpr IdleState hltIdleState{
		.name = "HLT",
		.exitLatency = 0,
		.targetResidency = 0,
		.losesTlb = false,
		.method = {}
	};
}

frg::span<const IdleState> getIdleStates() {
	return {&hltIdleState, 1};
}

void idleUntilInterrupt(const IdleMethod &) {
	assert(!intsAreEnabled());
	// sti only takes effect after the next instruction, so no interrupt can be taken before hlt.
	asm volatile (
		"sti\n"
		"\thlt\n"
		"\tcli"
		::: "memory"
	);
}

} // namespace thor
