#include <x86/machine.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/arch/pmc-intel.hpp>

namespace thor {

enum class IntelCounter {
	none,
	fixed0, // Instructions retired.
	fixed1, // Clock cycles.
	fixed2, // TSC cycles.
};

static int fixedCtrIndex(IntelCounter ctr) {
	assert(ctr != IntelCounter::none);
	return static_cast<int>(ctr) - static_cast<int>(IntelCounter::fixed0);
}

static IntelCounter whichCounter = IntelCounter::fixed2;

// Bitmask of fixed PMCs supported by the CPU.
static uint32_t supportedFixedCounters{0};

// Bit width of PMCs.
static int counterBitWidth{0};

// This is hardcoded to yield a fixed number of events per second on a 1 GHz machine for now.
// TODO: We want to adaptively pick this to achieve a constant sampling rate.
static uint64_t initialCount = 4'000'000'000/1000;

void initializeIntelPmc() {
	auto c = common::x86::cpuid(0xA);
	auto version = c[0] & 0xFF;
	counterBitWidth = (c[0] >> 16) & 0xFF;
	infoLogger() << "Intel PMC version " << version << frg::endlog;
	infoLogger() << "    Counters are " << counterBitWidth << " bits" << frg::endlog;
	if (version < 2) {
		warningLogger() << "Fixed counters need at least Intel PMC version 2" << frg::endlog;
		return;
	}

	auto numFixed = c[3] & 0x1F;
	for (size_t i = 0; i < 31; ++i) {
		// Intel recommends this expression to check for fixed PMCs (see CPUID documentation).
		if ((c[2] & (UINT32_C(1) << i)) || numFixed > i) {
			infoLogger() << "    Fixed counter " << i << " is available" << frg::endlog;
			supportedFixedCounters |= 1 << i;
		}
	}

	// Disable all fixed performance counters.
	common::x86::wrmsr(0x38D, // PERF_FIXED_CTR_CTRL
		0
	);

	auto ctrIndex = fixedCtrIndex(whichCounter);
	if (!(supportedFixedCounters & (1 << ctrIndex))) {
		warningLogger() << "Fixed counter " << ctrIndex
				<< " was requested but is not supported by CPU" << frg::endlog;
		return;
	}

	// Counters first need to be enabled in the "global control" MSR.
	common::x86::wrmsr(0x38F, // PERF_GLOBAL_CTRL
			common::x86::rdmsr(0x38F)
			| (UINT64_C(1) << (32 + ctrIndex)));
}

void setIntelPmc() {
	auto ctrIndex = fixedCtrIndex(whichCounter);

	// Disable the performance counter.
	common::x86::wrmsr(0x38D, // PERF_FIXED_CTR_CTRL
		0
	);

	// Clear overflow.
	common::x86::wrmsr(0x390, // PERF_GLOBAL_OVF_CTRL
			UINT64_C(1) << (32 + ctrIndex));

	// Program the initial value.
	common::x86::wrmsr(0x309 + ctrIndex, // PREF_FIXED_CTRx (= PERF_FIXED_CTR0 to PERF_FIXED_CTR6).
			(UINT64_C(1) << counterBitWidth) - initialCount);

	// Enable the performance counter.
	// KVM requires this MSR write to happen *after* the initial value is set.
	common::x86::wrmsr(0x38D, // PERF_FIXED_CTR_CTRL
			(UINT64_C(3) << (0 + 4 * ctrIndex)) // User + supervisor mode for PERF_FIXED_CTRx
			| (UINT64_C(1) << (3 + 4 * ctrIndex)) // Enable PMI for PREF_FIXED_CTRx
	);
}

bool checkIntelPmcOverflow() {
	auto ctrIndex = fixedCtrIndex(whichCounter);
	return common::x86::rdmsr(0x38E) // PERF_GLOBAL_STATUS
			& (UINT64_C(1) << (32 + ctrIndex)); // Overflow of PERF_FIXED_CTRx
}

} // namespace thor
