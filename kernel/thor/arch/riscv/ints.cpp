#include <riscv/sbi.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

namespace {

bool raiseIpiBit(CpuData *dstData, uint64_t bit) {
	auto alreadyPending = dstData->pendingIpis.fetch_or(bit, std::memory_order_acq_rel);
	return !(alreadyPending & bit);
}

void doSendIpi(CpuData *dstData) {
	auto hartId = dstData->hartId;
	if (sbi::Error e = sbi::ipi::sendIpi(1, hartId); e)
		panicLogger() << "Failed to send ping IPI to HART " << hartId << " (error: " << e << ")"
		              << frg::endlog;
}

} // namespace

void sendPingIpi(CpuData *dstData) {
	if (raiseIpiBit(dstData, PlatformCpuData::ipiPing))
		doSendIpi(dstData);
}

void sendShootdownIpi() {
	// TODO: This implementation is sub-optimal since it calls N times into SBI.
	//       It would be possible to exploit the hart mask to reduce the number of SBI calls.
	// TODO: It would also be possible to reduce the number of fetch_or calls
	//       by tracking global counters for broadcast IPIs.
	for (size_t i = 0; i < getCpuCount(); ++i) {
		auto *dstData = getCpuData(i);
		if (raiseIpiBit(dstData, PlatformCpuData::ipiShootdown))
			doSendIpi(dstData);
	}
}

void sendSelfCallIpi() {
	auto *selfData = getCpuData();
	if (raiseIpiBit(selfData, PlatformCpuData::ipiSelfCall))
		doSendIpi(selfData);
}

void suspendSelf() {
	enableInts();
	while (true)
		asm volatile("wfi");
}

} // namespace thor
