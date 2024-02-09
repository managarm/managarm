#include <thor-internal/arch/gic.hpp>
#include <thor-internal/arch/gic_v2.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/dtb/dtb.hpp>

namespace thor {

Gic *gic;
static uint8_t gicVersion;

static initgraph::Task initGic{&globalInitEngine, "arm.init-gic",
	initgraph::Requires{getDeviceTreeParsedStage(), getBootProcessorReadyStage()},
	initgraph::Entails{getIrqControllerReadyStage()},
	// Initialize the GIC.
	[] {
		if (initGicV2()) {
			gicVersion = 2;
		} else {
			assert(!"Failed to find GIC");
		}
		initGicOnThisCpu();
	}
};

initgraph::Stage *getIrqControllerReadyStage() {
	static initgraph::Stage s{&globalInitEngine, "arm.irq-controller-ready"};
	return &s;
}

void initGicOnThisCpu() {
	if (gicVersion == 2) {
		initGicOnThisCpuV2();
	} else {
		assert(!"Unhandled gic version");
	}
}

}
