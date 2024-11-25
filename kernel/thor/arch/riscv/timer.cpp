#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

extern ClockSource *globalClockSource;
extern PrecisionTimerEngine *globalTimerEngine;

namespace {

struct RiscvClockSource : ClockSource {
	uint64_t currentNanos() override { return getRawTimestampCounter(); }
};

struct RiscvTimer : AlarmTracker {
	virtual void arm(uint64_t nanos) {}
};

constinit frg::manual_box<RiscvClockSource> riscvClockSource;
constinit frg::manual_box<RiscvTimer> riscvTimer;

} // namespace

static initgraph::Task initTimer{
    &globalInitEngine, "riscv.init-timer", initgraph::Entails{getTaskingAvailableStage()}, [] {
	    riscvClockSource.initialize();
	    riscvTimer.initialize();

	    globalClockSource = riscvClockSource.get();
	    globalTimerEngine =
	        frg::construct<PrecisionTimerEngine>(*kernelAlloc, globalClockSource, riscvTimer.get());
    }
};

} // namespace thor
