#include <thor-internal/arch/timer.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/initgraph.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/arch/gic.hpp>

namespace thor {

static uint64_t ticksPerSecond;

uint64_t getRawTimestampCounter() {
	uint64_t cntpct;
	asm volatile ("mrs %0, cntpct_el0" : "=r"(cntpct));
	return cntpct;
}

uint64_t getVirtualTimestampCounter() {
	uint64_t cntpct;
	asm volatile ("mrs %0, cntvct_el0" : "=r"(cntpct));
	return cntpct;
}

struct PhysicalGenericTimer : IrqSink, ClockSource {
	PhysicalGenericTimer()
	: IrqSink{frg::string<KernelAlloc>{*kernelAlloc, "physical-generic-timer-irq"}} { }

	IrqStatus raise() override {
		return IrqStatus::acked;
	}

	uint64_t currentNanos() override {
		return getRawTimestampCounter() * 1000000000 / ticksPerSecond;
	}
};

extern ClockSource *globalClockSource;
extern PrecisionTimerEngine *globalTimerEngine;

struct VirtualGenericTimer : IrqSink, AlarmTracker {
	VirtualGenericTimer()
	: IrqSink{frg::string<KernelAlloc>{*kernelAlloc, "virtual-generic-timer-irq"}} { }

	using AlarmTracker::fireAlarm;

	IrqStatus raise() override {
		fireAlarm();
		return IrqStatus::acked;
	}

	void arm(uint64_t nanos) override {
		uint64_t compare = getVirtualTimestampCounter() + ticksPerSecond * nanos / 1000000000;

		asm volatile ("msr cntv_cval_el0, %0" :: "r"(compare));
	}
};

frg::manual_box<PhysicalGenericTimer> globalPGTInstance;
frg::manual_box<VirtualGenericTimer> globalVGTInstance;

void initializeTimers() {
	asm volatile ("mrs %0, cntfrq_el0" : "=r"(ticksPerSecond));

	// enable and unmask generic timers
	asm volatile ("msr cntp_cval_el0, %0" :: "r"(0xFFFFFFFFFFFFFFFF));
	asm volatile ("msr cntp_ctl_el0, %0" :: "r"(uint64_t{1}));
	asm volatile ("msr cntv_cval_el0, %0" :: "r"(0xFFFFFFFFFFFFFFFF));
	asm volatile ("msr cntv_ctl_el0, %0" :: "r"(uint64_t{1}));
}

void armPreemption(uint64_t nanos) {
	uint64_t compare = getRawTimestampCounter() + ticksPerSecond * nanos / 1000000000;

	asm volatile ("msr cntp_cval_el0, %0" :: "r"(compare));
}

void disarmPreemption() {
	asm volatile ("msr cntp_cval_el0, %0" :: "r"(0xFFFFFFFFFFFFFFFF));
}

extern frg::manual_box<GicDistributor> dist;

static initgraph::Task initTimerIrq{&basicInitEngine, "arm.init-timer-irq",
	initgraph::Requires{getIrqControllerReadyStage()},
	[] {
		// TODO: this is a bit of a hack
		globalPGTInstance.initialize();
		globalClockSource = globalPGTInstance.get();

		globalVGTInstance.initialize();
		globalTimerEngine = frg::construct<PrecisionTimerEngine>(*kernelAlloc,
			globalClockSource, globalVGTInstance.get());

		// TODO: discover this via the dtb

		auto ppin = dist->setupIrq(30, TriggerMode::edge);
		IrqPin::attachSink(ppin, globalPGTInstance.get());

		auto vpin = dist->setupIrq(27, TriggerMode::edge);
		IrqPin::attachSink(vpin, globalVGTInstance.get());
	}
};

} // namespace thor
