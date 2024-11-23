#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

extern ClockSource *globalClockSource;

namespace {

struct RiscvClockSource : ClockSource {
	uint64_t currentNanos() override { return getRawTimestampCounter(); }
};

constinit frg::manual_box<RiscvClockSource> riscvClockSource;

} // namespace

static initgraph::Task initTimer{
    &globalInitEngine, "riscv.init-timer", initgraph::Entails{getTaskingAvailableStage()}, [] {
	    riscvClockSource.initialize();
	    globalClockSource = riscvClockSource.get();
    }
};

} // namespace thor
