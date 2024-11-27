#include <riscv/sbi.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

namespace {

void doSendIpi(CpuData *dstData) {
	auto hartId = dstData->hartId;
	if (sbi::Error e = sbi::ipi::sendIpi(1, hartId); e)
		panicLogger() << "Failed to send ping IPI to HART " << hartId << " (error: " << e << ")"
		              << frg::endlog;
}

} // namespace

void sendPingIpi(CpuData *dstData) {
	// TODO: Set an atomic flag to indicate which IPI was raised.
	doSendIpi(dstData);
}

void sendShootdownIpi() { unimplementedOnRiscv(); }

void sendSelfCallIpi() {
	// TODO: Set an atomic flag to indicate which IPI was raised.
	doSendIpi(getCpuData());
}

void suspendSelf() {
	enableInts();
	while (true)
		asm volatile("wfi");
}

} // namespace thor
