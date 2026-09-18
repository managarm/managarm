#include <riscv/sbi.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>

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
	if (dstData->cpuState.load(std::memory_order_acquire) != CpuState::online)
		return;

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

		// A stale non-online state would wrongly skip a CPU, hence re-check behind a fence.
		// This pairs with the fence in setCpuState().
		if (dstData->cpuState.load(std::memory_order_acquire) != CpuState::online) [[unlikely]] {
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if (dstData->cpuState.load(std::memory_order_seq_cst) != CpuState::online)
				continue;
		}

		if (raiseIpiBit(dstData, PlatformCpuData::ipiShootdown))
			doSendIpi(dstData);
	}
}

void sendSelfCallIpi() {
	auto *selfData = getCpuData();
	if (raiseIpiBit(selfData, PlatformCpuData::ipiSelfCall))
		doSendIpi(selfData);
}

void sendHypervisorIpi(CpuData *dstData) {
	if (dstData->cpuState.load(std::memory_order_acquire) != CpuState::online)
		return;

	if (raiseIpiBit(dstData, PlatformCpuData::ipiHypervisor))
		doSendIpi(dstData);
}

void haltUntilInterrupt() {
	assert(!intsAreEnabled());
	// Wait for an interrupt with interrupts masked.
	asm volatile ("wfi" ::: "memory");
	// Flush the pending interrupt.
	enableInts();
	disableInts();
}

} // namespace thor
