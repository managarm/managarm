#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/gic_v2.hpp>
#include <thor-internal/arch/gic_v3.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/metrics.hpp>

namespace thor {

void sendPingIpi(CpuData *dstData) {
	pingIpisSentCounter.add();
	std::visit(
	    frg::overloaded{
	        [](std::monostate) {
		        panicLogger() << "thor: Cannot send IPIs without an IRQ controller" << frg::endlog;
		        __builtin_unreachable();
	        },
	        [&](GicV2 *gic) { gic->sendIpi(dstData->cpuIndex, 0); },
	        [&](GicV3 *gic) { gic->sendIpi(dstData->cpuIndex, 0); },
	    },
	    externalIrq
	);
}

void sendShootdownIpi() {
	size_t numTargets = 0;
	std::visit(
	    frg::overloaded{
	        [](std::monostate) {
		        panicLogger() << "thor: Cannot send IPIs without an IRQ controller" << frg::endlog;
		        __builtin_unreachable();
	        },
	        [&](GicV2 *gic) { numTargets = gic->sendIpiToOthers(1); },
	        [&](GicV3 *gic) { numTargets = gic->sendIpiToOthers(1); },
	    },
	    externalIrq
	);
	shootdownIpisSentCounter.add(numTargets);
}

void sendShootdownIpi(const frg::dyn_bitset<KernelAlloc> &targets) {
	size_t numTargets = 0;
	std::visit(
	    frg::overloaded{
	        [](std::monostate) {
		        panicLogger() << "thor: Cannot send IPIs without an IRQ controller" << frg::endlog;
		        __builtin_unreachable();
	        },
	        [&](GicV2 *gic) { numTargets = gic->sendIpi(targets, 1); },
	        [&](GicV3 *gic) { numTargets = gic->sendIpi(targets, 1); },
	    },
	    externalIrq
	);
	shootdownIpisSentCounter.add(numTargets);
}

void sendSelfCallIpi() {
	selfCallIpisSentCounter.add();
	auto *dstData = getCpuData();
	std::visit(
	    frg::overloaded{
	        [](std::monostate) {
		        panicLogger() << "thor: Cannot send IPIs without an IRQ controller" << frg::endlog;
		        __builtin_unreachable();
	        },
	        [&](GicV2 *gic) { gic->sendIpi(dstData->cpuIndex, 2); },
	        [&](GicV3 *gic) { gic->sendIpi(dstData->cpuIndex, 2); },
	    },
	    externalIrq
	);
}

} // namespace thor
