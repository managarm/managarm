#include <frigg/arch_x86/machine.hpp>
#include "pmc-intel.hpp"

namespace thor {

enum class IntelCounter {
	none,
	fixed0, // Instructions retired.
	fixed1 // Clock cycles.
};

static IntelCounter whichCounter = IntelCounter::fixed1;

void initializeIntelPmc() {
	// Disable all fixed performance counters.
	frigg::arch_x86::wrmsr(0x38D, // PERF_FIXED_CTR_CTRL
		0
	);

	// Counters first need to be enabled in the "global control" MSR.
	frigg::arch_x86::wrmsr(0x38F, // PERF_GLOBAL_CTRL
			frigg::arch_x86::rdmsr(0x38F)
			| (UINT64_C(1) << 32)
			| (UINT64_C(1) << 33));
}

void setIntelPmc() {
	// Disable the performance counter.
	frigg::arch_x86::wrmsr(0x38D, // PERF_FIXED_CTR_CTRL
		0
	);

	// Program the initial value.
	// This is hardcoded to yield a fixed number of events per second on a 1 GHz machine for now.
	if(whichCounter == IntelCounter::fixed0) {
		frigg::arch_x86::wrmsr(0x309, // PERF_FIXED_CTR0
				(UINT64_C(1) << 48) - 1'000'000'000/5000);
	}else{
		assert(whichCounter == IntelCounter::fixed1);
		frigg::arch_x86::wrmsr(0x30A, // PERF_FIXED_CTR1
				(UINT64_C(1) << 48) - 1'000'000'000/5000);
	}

	// TODO: Clear overflow. This is required for real hardware.
	//frigg::arch_x86::wrmsr(0x390, // PERF_GLOBAL_OVF_CTRL
	//		UINT64_C(1) << 32);
	// TODO: This is what example code on the internet does, but it looks wrong:
	//		//frigg::arch_x86::rdmsr(0x390) & ~(UINT64_C(1) << 32));

	// Enable the performance counter.
	// KVM requires this MSR write to happen *after* the initial value is set.
	if(whichCounter == IntelCounter::fixed0) {
		frigg::arch_x86::wrmsr(0x38D, // PERF_FIXED_CTR_CTRL
				(UINT64_C(3) << 0) // User + supervisor mode for PERF_FIXED_CTR0
				| (UINT64_C(1) << 3) // Enable PMI for PREF_FIXED_CTR0
		);
	}else{
		assert(whichCounter == IntelCounter::fixed1);
		frigg::arch_x86::wrmsr(0x38D, // PERF_FIXED_CTR_CTRL
				(UINT64_C(3) << 4) // User + supervisor mode for PERF_FIXED_CTR1
				| (UINT64_C(1) << 7) // Enable PMI for PREF_FIXED_CTR1
		);
	}
}

bool checkIntelPmcOverflow() {
	if(whichCounter == IntelCounter::fixed0) {
		frigg::arch_x86::wrmsr(0x309, // PERF_FIXED_CTR0
				(UINT64_C(1) << 48) - 1'000'000'000/5000);
		return frigg::arch_x86::rdmsr(0x38E) // PERF_GLOBAL_STATUS
				& (UINT64_C(1) << 32); // Overflow of PERF_FIXED_CTR0
	}else{
		assert(whichCounter == IntelCounter::fixed1);
		return frigg::arch_x86::rdmsr(0x38E) // PERF_GLOBAL_STATUS
				& (UINT64_C(1) << 33); // Overflow of PERF_FIXED_CTR1
	}
}

} // namespace thor
