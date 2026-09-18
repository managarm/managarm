#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/gic_v2.hpp>
#include <thor-internal/arch/gic_v3.hpp>
#include <thor-internal/arch/trap.hpp>

namespace thor {

void haltUntilInterrupt() {
	assert(!intsAreEnabled());
	// Wait for an interrupt with interrupts masked.
	asm volatile ("wfi" ::: "memory");
	// Flush the pending interrupt.
	enableInts();
	disableInts();
}

void sendPingIpi(CpuData *dstData) {
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
	std::visit(
	    frg::overloaded{
	        [](std::monostate) {
		        panicLogger() << "thor: Cannot send IPIs without an IRQ controller" << frg::endlog;
		        __builtin_unreachable();
	        },
	        [&](GicV2 *gic) { gic->sendIpiToOthers(1); },
	        [&](GicV3 *gic) { gic->sendIpiToOthers(1); },
	    },
	    externalIrq
	);
}

void sendSelfCallIpi() {
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
