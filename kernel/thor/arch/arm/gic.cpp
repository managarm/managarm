#include <thor-internal/arch/gic.hpp>
#include <thor-internal/arch/gic_v2.hpp>
#include <thor-internal/arch/gic_v3.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/int-call.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/schedule.hpp>

namespace thor {

static initgraph::Task initGic{
    &globalInitEngine,
    "arm.init-gic",
    initgraph::Requires{getDeviceTreeParsedStage(), getBootProcessorReadyStage()},
    initgraph::Entails{getIrqControllerReadyStage()},
    // Initialize the GIC.
    [] {
	    if (initGicV2() || initGicV3()) {
		    initGicOnThisCpu();
	    }
    }
};

void initGicOnThisCpu() {
	if (std::holds_alternative<GicV2 *>(externalIrq)) {
		initGicOnThisCpuV2();
	} else if (std::holds_alternative<GicV3 *>(externalIrq)) {
		initGicOnThisCpuV3();
	}
}

constexpr bool logSGIs = false;
constexpr bool logSpurious = false;

void handleIrq(IrqImageAccessor image, IrqPin *irq);

void handleGicIrq(IrqImageAccessor image, ClaimedExternalIrq irq) {
	auto *cpuData = getCpuData();

	if (irq.irq < 16) {
		if constexpr (logSGIs) {
			infoLogger() << "thor: handleGicIrq: on CPU " << cpuData->cpuIndex << ", got an SGI "
			             << irq.irq << frg::endlog;
		}

		std::visit(
		    frg::overloaded{
		        [&](GicV2 *gic) { gic->eoi(irq.cpu, irq.irq); },
		        [&](GicV3 *gic) { gic->eoi(irq.cpu, irq.irq); },
		        [](auto &&) {
			        // How did we even get here..?
			        __builtin_trap();
		        }
		    },
		    externalIrq
		);

		if (irq.irq == 0) {
			localScheduler.get(cpuData).forcePreemptionCall();
		} else if (irq.irq == 1) {
			assert(!irqMutex().nesting());
			disableUserAccess();

			for (auto &binding : asidData.get()->bindings) {
				binding.shootdown();
			}

			asidData.get()->globalBinding.shootdown();
		} else if (irq.irq == 2) {
			assert(!irqMutex().nesting());
			disableUserAccess();

			SelfIntCallBase::runScheduledCalls();
		} else {
			panicLogger() << "thor: handleGicIrq: Received unexpected SGI " << irq.irq
			              << frg::endlog;
		}

		localScheduler.get(cpuData).checkPreemption(image);
	} else if (irq.irq >= 1020) {
		if constexpr (logSpurious) {
			infoLogger() << "thor: handleGicIrq: on CPU " << cpuData->cpuIndex
			             << ", got a spurious IRQ " << irq.irq << frg::endlog;
		}
	} else {
		handleIrq(image, irq.pin);
	}
}

} // namespace thor
