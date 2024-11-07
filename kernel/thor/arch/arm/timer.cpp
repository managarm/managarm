#include <thor-internal/arch/timer.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/schedule.hpp>
#include <initgraph.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/arch/gic.hpp>
#include <thor-internal/dtb/dtb.hpp>

namespace thor {

static uint64_t ticksPerSecond;
static uint64_t ticksPerMilli;

uint64_t getRawTimestampCounter() {
	uint64_t cntpct;
	asm volatile ("mrs %0, cntpct_el0" : "=r"(cntpct));
	return cntpct;
}

uint64_t getVirtualTimestampCounter() {
	uint64_t cntvct;
	asm volatile ("mrs %0, cntvct_el0" : "=r"(cntvct));
	return cntvct;
}

struct PhysicalGenericTimer : IrqSink, ClockSource {
	PhysicalGenericTimer()
	: IrqSink{frg::string<KernelAlloc>{*kernelAlloc, "physical-generic-timer-irq"}} { }

	virtual ~PhysicalGenericTimer() = default;

	IrqStatus raise() override {
		disarmPreemption();
		return IrqStatus::acked;
	}

	uint64_t currentNanos() override {
		return getRawTimestampCounter() * 1000000 / ticksPerMilli;
	}
};

extern ClockSource *globalClockSource;
extern PrecisionTimerEngine *globalTimerEngine;

struct VirtualGenericTimer : IrqSink, AlarmTracker {
	VirtualGenericTimer()
	: IrqSink{frg::string<KernelAlloc>{*kernelAlloc, "virtual-generic-timer-irq"}} { }

	virtual ~VirtualGenericTimer() = default;

	using AlarmTracker::fireAlarm;

	IrqStatus raise() override {
		disarm();
		fireAlarm();
		return IrqStatus::acked;
	}

	void arm(uint64_t deadline) override {
		if (!deadline) {
			disarm();
			return;
		}

		auto now = systemClockSource()->currentNanos();
		auto diff = deadline - now;

		if (deadline < now) {
			diff = 0;
		}

		uint64_t compare = getVirtualTimestampCounter() + ticksPerSecond * diff / 1000000000;

		asm volatile ("msr cntv_cval_el0, %0" :: "r"(compare));
	}

	void disarm() {
		asm volatile ("msr cntv_cval_el0, %0" :: "r"(0xFFFFFFFFFFFFFFFF));
	}
};

frg::manual_box<PhysicalGenericTimer> globalPGTInstance;
frg::manual_box<VirtualGenericTimer> globalVGTInstance;

void initializeTimers() {
	asm volatile ("mrs %0, cntfrq_el0" : "=r"(ticksPerSecond));
	ticksPerMilli = ticksPerSecond / 1000;

	// enable and unmask generic timers
	asm volatile ("msr cntp_cval_el0, %0" :: "r"(0xFFFFFFFFFFFFFFFF));
	asm volatile ("msr cntp_ctl_el0, %0" :: "r"(uint64_t{1}));
	asm volatile ("msr cntv_cval_el0, %0" :: "r"(0xFFFFFFFFFFFFFFFF));
	asm volatile ("msr cntv_ctl_el0, %0" :: "r"(uint64_t{1}));
}

void armPreemption(uint64_t nanos) {
	uint64_t compare = getRawTimestampCounter() + ticksPerSecond * nanos / 1000000000;

	asm volatile ("msr cntp_cval_el0, %0" :: "r"(compare));
	getCpuData()->preemptionIsArmed = true;
}

void disarmPreemption() {
	asm volatile ("msr cntp_cval_el0, %0" :: "r"(0xFFFFFFFFFFFFFFFF));
	getCpuData()->preemptionIsArmed = false;
}

bool preemptionIsArmed() {
	return getCpuData()->preemptionIsArmed;
}

static bool timersFound = false;

static DeviceTreeNode *timerNode = nullptr;

static initgraph::Task initTimerIrq{&globalInitEngine, "arm.init-timer-irq",
	initgraph::Requires{getIrqControllerReadyStage()},
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		globalPGTInstance.initialize();
		globalClockSource = globalPGTInstance.get();

		globalVGTInstance.initialize();
		globalTimerEngine = frg::construct<PrecisionTimerEngine>(*kernelAlloc,
			globalClockSource, globalVGTInstance.get());

		getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
			if (node->isCompatible<1>({"arm,armv8-timer"})) {
				timerNode = node;
				return true;
			}

			return false;
		});

		assert(timerNode && "Failed to find timer");

		// These offsets are defined in the Linux DTB binding for compatible nodes
		auto irqPhys = timerNode->irqs()[1];
		auto irqVirt = timerNode->irqs()[2];

		auto ppin = gic->setupIrq(irqPhys.id, irqPhys.trigger);
		IrqPin::attachSink(ppin, globalPGTInstance.get());

		auto vpin = gic->setupIrq(irqVirt.id, irqVirt.trigger);
		IrqPin::attachSink(vpin, globalVGTInstance.get());

		timersFound = true;
	}
};

bool haveTimer() {
	return timersFound;
}

// Sets up the proper interrupt trigger and polarity for the PPI
void initTimerOnThisCpu() {
	auto irqPhys = timerNode->irqs()[1];
	auto physPin = gic->getPin(irqPhys.id);
	physPin->setMode(irqPhys.trigger, irqPhys.polarity);

	auto irqVirt = timerNode->irqs()[2];
	auto virtPin = gic->getPin(irqVirt.id);
	virtPin->setMode(irqVirt.trigger, irqVirt.polarity);
}

} // namespace thor
