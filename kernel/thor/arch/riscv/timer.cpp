#include <riscv/sbi.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch/timer.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/util.hpp>

namespace thor {

// TODO: Move declaration to header.
void handlePreemption(IrqImageAccessor image);

extern ClockSource *globalClockSource;
extern PrecisionTimerEngine *globalTimerEngine;

namespace {

// Frequency and inverse frequency of the CPU timer in nHz and ns, respectively.
constinit FreqFraction freq;
constinit FreqFraction inverseFreq;

void updateSmodeTimer() {
	assert(!intsAreEnabled());
	auto *cpuData = getCpuData();

	auto deadline = ~UINT64_C(0);
	if (cpuData->timerDeadline)
		deadline = frg::min(deadline, *cpuData->timerDeadline);
	if (cpuData->preemptionDeadline)
		deadline = frg::min(deadline, *cpuData->preemptionDeadline);

	// Avoid an SBI call if the deadline did not change.
	if (deadline == cpuData->currentDeadline)
		return;
	cpuData->currentDeadline = deadline;

	if (riscvHartCapsNote->hasExtension(RiscvExtension::sstc)) {
		riscv::writeCsr<riscv::Csr::stimecmp>(deadline);
	} else {
		sbi::time::setTimer(deadline);
	}
}

struct RiscvClockSource : ClockSource {
	uint64_t currentNanos() override { return inverseFreq * getRawTimestampCounter(); }
};

struct RiscvTimer : AlarmTracker {
	using AlarmTracker::fireAlarm;

	virtual void arm(uint64_t nanos) {
		assert(!intsAreEnabled());
		getCpuData()->timerDeadline = freq * nanos;
		updateSmodeTimer();
	}
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

bool haveTimer() { return static_cast<bool>(freq); }

bool preemptionIsArmed() {
	assert(!intsAreEnabled());
	return static_cast<bool>(getCpuData()->preemptionDeadline);
}

void armPreemption(uint64_t nanos) {
	assert(!intsAreEnabled());
	getCpuData()->preemptionDeadline = getRawTimestampCounter() + freq * nanos;
	updateSmodeTimer();
}

void disarmPreemption() {
	assert(!intsAreEnabled());
	getCpuData()->preemptionDeadline = frg::null_opt;
	updateSmodeTimer();
}

void onTimerInterrupt(IrqImageAccessor image) {
	auto *cpuData = getCpuData();
	auto tsc = getRawTimestampCounter();

	// Clear all deadlines that have expired.
	auto checkAndClear = [&](frg::optional<uint64_t> &deadline) -> bool {
		if (!deadline || tsc < *deadline)
			return false;
		deadline = frg::null_opt;
		return true;
	};

	auto timerExpired = checkAndClear(cpuData->timerDeadline);
	auto preemptionExpired = checkAndClear(cpuData->preemptionDeadline);

	// Update the timer hardware.
	updateSmodeTimer();

	// Finally, take action for the deadlines that have expired.
	// Note that preemption has to be done last.

	if (timerExpired)
		riscvTimer->fireAlarm();

	if (preemptionExpired)
		handlePreemption(image);
}

} // namespace thor
