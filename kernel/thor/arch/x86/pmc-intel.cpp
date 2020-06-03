#include <frigg/arch_x86/machine.hpp>
#include "pmc-intel.hpp"

namespace thor {

void initializeIntelPmc() {
	// Counters first need to be enabled in the "global control" MSR.
	frigg::arch_x86::wrmsr(0x38F, // PERF_GLOBAL_CTRL
			frigg::arch_x86::rdmsr(0x38F)
			| (UINT64_C(1) << 32));
}

void setIntelPmc() {
	// Disable the performance counter.
	frigg::arch_x86::wrmsr(0x38D, // PERF_FIXED_CTR_CTRL
		0
	);

	// Program the initial value.
	frigg::arch_x86::wrmsr(0x309, // PERF_FIXED_CTR0
			(UINT64_C(1) << 48) - 0x1000);

	// TODO: Clear overflow. This is required for real hardware.
	//frigg::arch_x86::wrmsr(0x390, // PERF_GLOBAL_OVF_CTRL
	//		UINT64_C(1) << 32);
	// TODO: This is what example code on the internet does, but it looks wrong:
	//		//frigg::arch_x86::rdmsr(0x390) & ~(UINT64_C(1) << 32));

	// Enable the performance counter.
	// KVM requires this MSR write to happen *after* the initial value is set.
	frigg::arch_x86::wrmsr(0x38D, // PERF_FIXED_CTR_CTRL
			UINT64_C(3) // User + supervisor mode.
			| UINT64_C(8) // Enable PMI
	);
}

bool checkIntelPmcOverflow() {
	return frigg::arch_x86::rdmsr(0x38E) // PERF_GLOBAL_STATUS
			& (UINT64_C(1) << 32);
}

} // namespace thor
