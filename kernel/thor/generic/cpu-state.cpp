#include <frg/eternal.hpp>
#include <thor-internal/cpu-state.hpp>

namespace thor {

namespace {

frg::eternal<async::recurring_event> cpuStateEventSingleton;

} // namespace anonymous

async::recurring_event &cpuStateEvent() { return cpuStateEventSingleton.get(); }

void setCpuState(CpuData *cpuData, CpuState state) {
	cpuData->cpuState.store(state, std::memory_order_seq_cst);
	// Pairs with the fence in RcuEngine::barrier():
	// either barrier() observes that we left the offline state, or we observe all stores that precede barrier().
	std::atomic_thread_fence(std::memory_order_seq_cst);
	// We cannot raise before we hit online since the IRQ controllers needed to send IPIs are not up yet.
	if (state == CpuState::online)
		cpuStateEvent().raise();
}

} // namespace thor
