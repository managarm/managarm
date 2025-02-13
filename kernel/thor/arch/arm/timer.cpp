#include <thor-internal/arch/timer.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch-generic/timer.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/schedule.hpp>
#include <initgraph.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/arch/gic.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/util.hpp>

namespace thor {

// Timer frequency and it's inverse stored in nHz and ns respectively.
constinit FreqFraction timerFreq;
constinit FreqFraction timerInverseFreq;

uint64_t getRawTimestampCounter() {
	uint64_t cnt;
	asm volatile ("mrs %0, cntvct_el0" : "=r"(cnt));
	return cnt;
}

struct GenericTimer : IrqSink {
	GenericTimer()
	: IrqSink{frg::string<KernelAlloc>{*kernelAlloc, "generic-timer-irq"}} { }

	virtual ~GenericTimer() = default;

	IrqStatus raise() override {
		handleTimerInterrupt();
		return IrqStatus::acked;
	}
};

frg::manual_box<GenericTimer> globalTimerSink;

uint64_t getClockNanos() {
	return timerInverseFreq * getRawTimestampCounter();
}

void setTimerDeadline(frg::optional<uint64_t> deadline) {
	if (deadline) {
		uint64_t rawDeadline = timerFreq * *deadline;

		asm volatile ("msr cntv_cval_el0, %0" :: "r"(rawDeadline));
		// Unmask the timer interrupt.
		asm volatile ("msr cntv_ctl_el0, %0" :: "r"(uint64_t{0b01}));
	} else {
		// Mask the timer interrupt.
		asm volatile ("msr cntv_ctl_el0, %0" :: "r"(uint64_t{0b11}));
	}
}

void initializeTimers() {
	constexpr uint64_t divisor = 1'000'000'000;
	uint64_t freqHz;
	asm volatile ("mrs %0, cntfrq_el0" : "=r"(freqHz));

	// Divide by 10^9 to convert Hz to nHz.
	timerFreq = computeFreqFraction(freqHz, divisor);
	timerInverseFreq = computeFreqFraction(divisor, freqHz);

	// Enable and mask the timer interrupt.
	asm volatile ("msr cntv_ctl_el0, %0" :: "r"(uint64_t{0b11}));
}

static bool timersFound = false;

static DeviceTreeNode *timerNode = nullptr;

static dt::IrqController *timerIrqParent = nullptr;
static frg::manual_box<dtb::Cells> timerIrq;

static initgraph::Task initTimerIrq{&globalInitEngine, "arm.init-timer-irq",
	initgraph::Requires{getIrqControllerReadyStage()},
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		globalTimerSink.initialize();

		getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
			if (node->isCompatible<1>({"arm,armv8-timer"})) {
				timerNode = node;
				return true;
			}

			return false;
		});

		assert(timerNode && "Failed to find timer");

		// TODO(qookie): I think Linux has some logic to pick
		// either the physical or virtual timer, which we
		// should probably replicate instead of always picking
		// the virtual one.

		int idx = 0;
		bool success = dt::walkInterrupts(
			[&] (DeviceTreeNode *parentNode, dtb::Cells irqCells) {
				// This offset is defined in the Linux
				// DTB binding for compatible nodes.
				if (idx == 2) {
					timerIrqParent = parentNode->getAssociatedIrqController();
					timerIrq.initialize(irqCells);
				}

				idx++;
			}, timerNode);

		assert(success && "Failed to parse generic timer interrupts");

		auto pin = timerIrqParent->resolveDtIrq(*timerIrq);
		IrqPin::attachSink(pin, globalTimerSink.get());

		timersFound = true;
	}
};

bool haveTimer() {
	return timersFound;
}

// Sets up the proper interrupt trigger and polarity for the PPI
void initTimerOnThisCpu() {
	auto pin = timerIrqParent->resolveDtIrq(*timerIrq);
	(void)pin;
}

} // namespace thor
