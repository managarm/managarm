#include <thor-internal/arch/idle-intel.hpp>
#include <thor-internal/debug.hpp>
#include <x86/machine.hpp>

namespace thor {

namespace {
	// Bit of kMsrIa32PowerCtl that lets the CPU promote C1 requests to C1E.
	constexpr uint64_t powerCtlC1ePromotion = uint64_t(1) << 1;

	// The tables below are taken from Linux' intel_idle driver (drivers/idle/intel_idle.c).
	// CPUID enumerates the hints but not their latencies.

	// Linux: skl_cstates.
	constexpr MwaitState kabyLakeStates[] = {
		{.name = "C1", .hint = 0x00, .exitLatency = 2'000, .targetResidency = 2'000, .losesTlb = false},
		{.name = "C1E", .hint = 0x01, .exitLatency = 10'000, .targetResidency = 20'000, .losesTlb = false},
		{.name = "C3", .hint = 0x10, .exitLatency = 70'000, .targetResidency = 100'000, .losesTlb = true},
		{.name = "C6", .hint = 0x20, .exitLatency = 85'000, .targetResidency = 200'000, .losesTlb = true},
		{.name = "C7s", .hint = 0x33, .exitLatency = 124'000, .targetResidency = 800'000, .losesTlb = true},
		{.name = "C8", .hint = 0x40, .exitLatency = 200'000, .targetResidency = 800'000, .losesTlb = true},
		{.name = "C9", .hint = 0x50, .exitLatency = 480'000, .targetResidency = 5'000'000, .losesTlb = true},
		{.name = "C10", .hint = 0x60, .exitLatency = 890'000, .targetResidency = 5'000'000, .losesTlb = true},
	};

	// Linux: ptl_cstates.
	constexpr MwaitState pantherLakeStates[] = {
		{.name = "C1", .hint = 0x00, .exitLatency = 1'000, .targetResidency = 1'000, .losesTlb = false},
		{.name = "C1E", .hint = 0x01, .exitLatency = 10'000, .targetResidency = 10'000, .losesTlb = false},
		{.name = "C6S", .hint = 0x21, .exitLatency = 300'000, .targetResidency = 300'000, .losesTlb = true},
		{.name = "C10", .hint = 0x60, .exitLatency = 370'000, .targetResidency = 2'500'000, .losesTlb = true},
	};

	struct MwaitModel {
		// All models below are in family 6.
		uint32_t model;
		frg::span<const MwaitState> states;
		// Do not promote C1 to C1E. Otherwise, the CPU may enter C1E although we request C1.
		bool disableC1ePromotion;
	};

	constexpr MwaitModel mwaitModels[] = {
		// Linux: INTEL_KABYLAKE_L.
		{.model = 0x8E, .states = {kabyLakeStates, sizeof(kabyLakeStates) / sizeof(MwaitState)},
				.disableC1ePromotion = true},
		// Linux: INTEL_KABYLAKE.
		{.model = 0x9E, .states = {kabyLakeStates, sizeof(kabyLakeStates) / sizeof(MwaitState)},
				.disableC1ePromotion = true},
		// Linux: INTEL_PANTHERLAKE_L.
		{.model = 0xCC, .states = {pantherLakeStates, sizeof(pantherLakeStates) / sizeof(MwaitState)},
				.disableC1ePromotion = false},
	};

	const MwaitModel *findMwaitModel(uint32_t family, uint32_t model) {
		if(family != 6)
			return nullptr;
		for(auto &mwaitModel : mwaitModels) {
			if(mwaitModel.model == model)
				return &mwaitModel;
		}
		return nullptr;
	}
}

frg::span<const MwaitState> initializeIntelMwaitStates() {
	auto signature = common::x86::cpuid(0x01)[0];
	auto family = (signature >> 8) & 0xF;
	auto model = (signature >> 4) & 0xF;
	// The extended family only applies to family 15 and the extended model only to families 6 and 15.
	if(family == 0xF)
		family += (signature >> 20) & 0xFF;
	if(family == 6 || family >= 0xF)
		model |= ((signature >> 16) & 0xF) << 4;

	const MwaitModel *mwaitModel = findMwaitModel(family, model);
	if(!mwaitModel) {
		debugLogger() << "thor: No C-state table for Intel family " << family
				<< ", model 0x" << frg::hex_fmt{model} << frg::endlog;
		return {};
	}

	// Hypervisors do not necessarily implement these MSRs.
	bool haveHypervisor = common::x86::cpuid(0x01)[2] & (uint32_t(1) << 31);
	if(!haveHypervisor) {
		auto config = common::x86::rdmsr(common::x86::kMsrPkgCstConfigControl);
		debugLogger() << "thor: Package C-state limit is " << (config & 0xF)
				<< ((config & (uint64_t(1) << 15)) ? " (locked)" : " (unlocked)") << frg::endlog;
	}

	if(mwaitModel->disableC1ePromotion && !haveHypervisor) {
		auto powerCtl = common::x86::rdmsr(common::x86::kMsrIa32PowerCtl);
		common::x86::wrmsr(common::x86::kMsrIa32PowerCtl, powerCtl & ~powerCtlC1ePromotion);
		debugLogger() << "thor: Disabled C1E promotion" << frg::endlog;
	}

	return mwaitModel->states;
}

} // namespace thor
