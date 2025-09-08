#include <riscv/sbi.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch-generic/timer.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch/timer.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/schedule.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/util.hpp>

namespace thor {

namespace {

// Frequency and inverse frequency of the CPU timer in nHz and ns, respectively.
constinit FreqFraction freq;
constinit FreqFraction inverseFreq;

initgraph::Task initTimer{
    &globalInitEngine,
    "riscv.init-timer",
    initgraph::Requires{getDeviceTreeParsedStage()},
    initgraph::Entails{getTaskingAvailableStage()},
    [] {
	    // Get the timebase-frequency property in /cpus.
	    auto *dtCpus = getDeviceTreeNodeByPath("/cpus");
	    if (!dtCpus)
		    panicLogger() << "Device tree node /cpus is not available" << frg::endlog;
	    auto maybeFreqProp = dtCpus->dtNode().findProperty("timebase-frequency");
	    if (!maybeFreqProp)
		    panicLogger() << "Device tree property timebase-frequency is missing from /cpus"
		                  << frg::endlog;
	    if (maybeFreqProp->size() != 4)
		    panicLogger() << "Expected exactly one u32 in timebase-frequency" << frg::endlog;
	    auto freqSeconds = maybeFreqProp->asU32();

	    const char *impl;
	    if (riscvHartCapsNote->hasExtension(RiscvExtension::sstc)) {
		    impl = "Sstc";
	    } else {
		    impl = "SBI";
	    }
	    infoLogger() << "thor: Using " << impl << " to update S-mode timer" << frg::endlog;
	    infoLogger() << "thor: Timer frequency is " << freqSeconds << " Hz" << frg::endlog;

	    // Frequency is given in Hz. Hence, we need to divide by 10^9 to convert to nHz.
	    uint64_t divisor = 1'000'000'000;
	    freq = computeFreqFraction(freqSeconds, divisor);
	    inverseFreq = computeFreqFraction(divisor, freqSeconds);
    }
};

} // namespace

uint64_t getRawTimestampCounter() {
	uint64_t v;
	asm volatile("rdtime %0" : "=r"(v));
	return v;
}

uint64_t getClockNanos() { return inverseFreq * getRawTimestampCounter(); }

void setTimerDeadline(frg::optional<uint64_t> deadline) {
	assert(!intsAreEnabled());

	uint64_t rawDeadline = ~UINT64_C(0);
	if (deadline)
		rawDeadline = freq * (*deadline);

	if (riscvHartCapsNote->hasExtension(RiscvExtension::sstc)) {
		riscv::writeCsr<riscv::Csr::stimecmp>(rawDeadline);
	} else {
		sbi::time::setTimer(rawDeadline);
	}
}

bool haveTimer() { return static_cast<bool>(freq); }

} // namespace thor
