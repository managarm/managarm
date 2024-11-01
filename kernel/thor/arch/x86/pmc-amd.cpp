#include <thor-internal/arch/pmc-amd.hpp>
#include <x86/machine.hpp>

namespace thor {

namespace counters {
inline constexpr unsigned int clockCycles = 0x76;
inline constexpr unsigned int instructionsRetired = 0xC0;
} // namespace counters

void setAmdPmc() {
	// TODO: Support counters with IDs > 0xFF.
	unsigned int whichCounter = counters::clockCycles;

	// First, disable the performance counter.
	// The manual recommends this to avoid races during the inital value update.
	// Furthermore, KVM (but not real hardware) requires this to work!
	common::x86::wrmsr(
	    0xC001'0200,
	    static_cast<uint64_t>(whichCounter & 0xFF) | (UINT64_C(3) << 16) // Count all events
	        | (UINT64_C(1) << 20)                                        // Enable LAPIC interrupt
	);

	// Program the initial value.
	// This is hardcoded to yield a fixed number of events per second on a 1 GHz machine for now.
	common::x86::wrmsr(0xC001'0201, -static_cast<uint64_t>(1'000'000'000 / 5000));

	// Re-enable the performance counter.
	common::x86::wrmsr(
	    0xC001'0200,
	    static_cast<uint64_t>(whichCounter & 0xFF) | (UINT64_C(3) << 16) // Count all events
	        | (UINT64_C(1) << 20)                                        // Enable LAPIC interrupt
	        | (UINT64_C(1) << 22) // Enable performance counter
	);
}

// Check whether the 48-bit counter value is positive.
bool checkAmdPmcOverflow() {
	auto value = common::x86::rdmsr(0xC001'0201);
	return !(value & (UINT64_C(1) << 47));
}

} // namespace thor
