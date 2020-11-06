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

void sendPingIpi(int id) {
	thor::infoLogger() << "sendPingIpi is unimplemented" << frg::endlog;
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

extern "C" void onPlatformIrq(IrqImageAccessor image) {
	auto [cpu, irq] = cpuInterface->get();

	if (irq < 16) {
		// TODO: handle SGIs
		panicLogger() << "thor: onPlatformIrq: got a SGI (no. " << irq << "), don't know what to do! originated from cpu " << cpu << frg::endlog;
	}

	if (irq > 1020) {
		infoLogger() << "thor: spurious irq occured" << frg::endlog;
		// no need to EOI spurious irqs
		return;
	}

	handleIrq(image, irq);
}

} // namespace thor
