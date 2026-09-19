#include <riscv/sbi.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/cpu-state.hpp>
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

		if (suppressIpiToOfflineCpu(dstData))
			continue;

		if (raiseIpiBit(dstData, PlatformCpuData::ipiShootdown))
			doSendIpi(dstData);
	}
}

void sendShootdownIpi(const frg::dyn_bitset<KernelAlloc> &targets) {
	// One SBI call reaches up to 64 harts whose IDs fall into the same aligned window.
	uint64_t base = 0;
	uint64_t hartMask = 0;
	auto flush = [&] {
		if (!hartMask)
			return;
		if (sbi::Error e = sbi::ipi::sendIpi(hartMask, base); e)
			panicLogger() << "Failed to send shootdown IPI to HARTs at " << base << " (error: " << e
			              << ")" << frg::endlog;
		hartMask = 0;
	};
	for (auto cpu : targets.set_bits()) {
		auto *dstData = getCpuData(cpu);

		if (suppressIpiToOfflineCpu(dstData))
			continue;

		if (!raiseIpiBit(dstData, PlatformCpuData::ipiShootdown))
			continue;
		auto hartId = dstData->hartId;
		if (hartMask && (hartId & ~UINT64_C(63)) != base)
			flush();
		base = hartId & ~UINT64_C(63);
		hartMask |= UINT64_C(1) << (hartId & 63);
	}
	flush();
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
	asm volatile("wfi" ::: "memory");
	// Flush the pending interrupt.
	enableInts();
	disableInts();
}

} // namespace thor
