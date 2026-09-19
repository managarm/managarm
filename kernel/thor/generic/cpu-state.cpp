#include <frg/eternal.hpp>
#include <thor-internal/arch-generic/asid.hpp>
#include <thor-internal/cpu-state.hpp>

namespace thor {

namespace {

frg::eternal<async::recurring_event> cpuStateEventSingleton;

} // namespace anonymous

async::recurring_event &cpuStateEvent() { return cpuStateEventSingleton.get(); }

void setCpuState(CpuData *cpuData, CpuState state) {
	// The shootdown drain below operates on the bindings of the current CPU.
	assert(cpuData == getCpuData());

	cpuData->cpuState.store(state, std::memory_order_seq_cst);
	// Pairs with the fences in RcuEngine::barrier() and in the shootdown IPI senders:
	// either they observe that we left the offline state, or we observe all stores that precede them.
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if (state == CpuState::online) {
		// Shootdowns that were initiated before we came online did not send us an IPI.
		for (auto &binding : asidData.get()->bindings)
			binding.shootdown();
		asidData.get()->globalBinding.shootdown();

		// We cannot raise before we hit online since the IRQ controllers needed to send IPIs are not up yet.
		cpuStateEvent().raise();
	}
}

} // namespace thor
