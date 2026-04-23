#include <dtb.hpp>
#include <eir-internal/cmdline.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>

namespace eir {

namespace {

initgraph::Task detectCpusFromDtb{
    &globalInitEngine, "dt.detect-cpu-count", initgraph::Entails{getKernelLoadableStage()}, [] {
	    if (!eirDtbPtr)
		    return;

	    DeviceTree dt{physToVirt<void>(eirDtbPtr)};
	    size_t cpuCount = 0;

	    dt.rootNode().discoverSubnodes(
	        [](auto node) { return frg::string_view(node.name()) == "cpus"; },
	        [&](auto cpusNode) {
		        cpusNode.discoverSubnodes(
		            [](auto node) {
			            auto name = frg::string_view(node.name());
			            return name.starts_with("cpu@");
		            },
		            [&](auto cpuNode) {
			            if (auto status = cpuNode.findProperty("status")) {
				            auto s = status->asString();
				            if (s.has_value() && *s != "okay")
					            return;
			            }
			            ++cpuCount;
		            }
		        );
	        }
	    );

	    if (cpuCount > 0) {
		    size_t smp = -1;
		    frg::array options = {frg::option{"smp", frg::as_number(smp)}};
		    parseCmdline(options);

		    size_t effectiveCpus = frg::min(cpuCount, smp);
		    cpuConfig.effectiveCpus = effectiveCpus;
		    cpuConfig.totalCpus = cpuCount;

		    auto log = infoLogger();
		    log << "eir: Detected " << cpuCount << " CPUs from MADT";
		    if (cpuCount != effectiveCpus)
			    log << " (but only using " << effectiveCpus << " CPUs)";
		    log << frg::endlog;
	    } else {
		    panicLogger() << "eir: Failed to detect CPUs from DT" << frg::endlog;
	    }
    }
};

} // namespace

} // namespace eir
