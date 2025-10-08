#include <thor-internal/arch/gic.hpp>
#include <thor-internal/arch/gic_v2.hpp>
#include <thor-internal/arch/gic_v3.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/main.hpp>

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

} // namespace thor
