#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/util.hpp>

namespace thor {

extern ClockSource *globalClockSource;
extern PrecisionTimerEngine *globalTimerEngine;

namespace {

// Frequency and inverse frequency of the CPU timer in nHz and ns, respectively.
constinit FreqFraction freq;
constinit FreqFraction inverseFreq;

struct RiscvClockSource : ClockSource {
	uint64_t currentNanos() override { return inverseFreq * getRawTimestampCounter(); }
};

struct RiscvTimer : AlarmTracker {
	virtual void arm(uint64_t nanos) {}
};

constinit frg::manual_box<RiscvClockSource> riscvClockSource;
constinit frg::manual_box<RiscvTimer> riscvTimer;

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
	    infoLogger() << "thor: Timer frequency is " << freqSeconds << " Hz" << frg::endlog;

	    // Frequency is given in Hz. Hence, we need to divide by 10^9 to convert to nHz.
	    uint64_t divisor = 1'000'000'000;
	    freq = computeFreqFraction(freqSeconds, divisor);
	    inverseFreq = computeFreqFraction(divisor, freqSeconds);

	    riscvClockSource.initialize();
	    riscvTimer.initialize();

	    globalClockSource = riscvClockSource.get();
	    globalTimerEngine =
	        frg::construct<PrecisionTimerEngine>(*kernelAlloc, globalClockSource, riscvTimer.get());
    }
};

} // namespace

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
