#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/gic.hpp>
#include <thor-internal/debug.hpp>
#include <assert.h>

namespace thor {

extern "C" void *thorExcVectors;

void initializeIrqVectors() {
	asm volatile ("msr vbar_el1, %0" :: "r"(&thorExcVectors));
}

void suspendSelf() { assert(!"Not implemented"); }

extern frg::manual_box<GicDistributor> dist;

void sendPingIpi(int id) {
	// TODO: The GIC cpu id *may* differ from the normal cpu id,
	// get the id from the GIC and store it in the cpu local data and
	// use that here
	dist->sendIpi(id, 0);
}

extern "C" void onPlatformInvalidException(FaultImageAccessor image) {
	thor::panicLogger() << "thor: an invalid exception has occured" << frg::endlog;
}

extern "C" void onPlatformSyncFault(FaultImageAccessor image) {
	infoLogger() << "onPlatformSyncFault" << frg::endlog;
	while(1);
}

extern "C" void onPlatformAsyncFault(FaultImageAccessor image) {
	infoLogger() << "onPlatformAsyncFault" << frg::endlog;
}

extern frg::manual_box<GicCpuInterface> cpuInterface;

void handleIrq(IrqImageAccessor image, int number);
void handlePreemption(IrqImageAccessor image);

static constexpr bool logSGIs = false;

extern "C" void onPlatformIrq(IrqImageAccessor image) {
	auto [cpu, irq] = cpuInterface->get();

	if (irq < 16) {
		if (logSGIs)
			infoLogger() << "thor: onPlatformIrq: got a SGI (no. " << irq << ") that originated from cpu " << cpu << frg::endlog;

		cpuInterface->eoi(cpu, irq);
		handlePreemption(image);
		return;
	}

	if (irq > 1020) {
		infoLogger() << "thor: spurious irq occured" << frg::endlog;
		// no need to EOI spurious irqs
		return;
	}

	handleIrq(image, irq);
}

} // namespace thor
