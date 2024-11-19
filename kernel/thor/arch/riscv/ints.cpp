#include <riscv/sbi.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

void sendPingIpi(int cpuIndex) {
	auto hartId = getCpuData(cpuIndex)->hartId;
	if (sbi::Error e = sbi::ipi::sendIpi(1, hartId); e)
		panicLogger() << "Failed to send ping IPI to HART " << hartId << " (error: " << e << ")"
		              << frg::endlog;
}

} // namespace thor
