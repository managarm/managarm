#include <string.h>

#include <frg/cmdline.hpp>
#include <thor-internal/arch-generic/idle.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/idle-intel.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/main.hpp>
#include <x86/machine.hpp>

namespace thor {

namespace {
	constexpr IdleState hltIdleState{
		.name = "hlt",
		.exitLatency = 0,
		.targetResidency = 0,
		.losesTlb = false,
		.method = {.instruction = IdleInstruction::hlt}
	};

	struct IdleCpuData {
		IdleState states[maxIdleStates];
		size_t numStates = 0;
		// Target of the monitor instruction. Stores to this cache line terminate mwait.
		alignas(64) uint64_t monitorWord = 0;
	};

	extern PerCpu<IdleCpuData> idleCpuData;
	THOR_DEFINE_PERCPU(idleCpuData);
}

void initializeIdleStates() {
	auto *data = &idleCpuData.get();
	auto addState = [&] (IdleState state) {
		assert(data->numStates < maxIdleStates);
		data->states[data->numStates++] = state;
	};

	frg::string_view idleMechanism = "mwait";
	frg::array args = {
		frg::option{"thor.idle", frg::as_string_view(idleMechanism)},
	};
	frg::parse_arguments(getKernelCmdline(), args);
	if(idleMechanism != "hlt" && idleMechanism != "mwait")
		warningLogger() << "thor: Ignoring unknown thor.idle=" << idleMechanism
				<< ", defaulting to mwait" << frg::endlog;

	// CPUID stores the vendor string in EBX, EDX, ECX (in this order).
	auto vendorLeaf = common::x86::cpuid(0);
	char vendor[13]{};
	memcpy(vendor, &vendorLeaf[1], 4);
	memcpy(vendor + 4, &vendorLeaf[3], 4);
	memcpy(vendor + 8, &vendorLeaf[2], 4);

	bool haveMwait = (common::x86::cpuid(0x01)[2] & (uint32_t(1) << 3))
			&& vendorLeaf[0] >= 5;
	// The sub-states in EDX of leaf 5 are only valid if ECX[0] is set.
	bool haveMwaitSubStates = haveMwait && (common::x86::cpuid(0x05)[2] & 1);

	// Only use mwait on CPUs for which we know the latencies of the C-states.
	frg::span<const MwaitState> mwaitStates;
	if(idleMechanism == "hlt") {
		debugLogger() << "thor: hlt is forced by thor.idle=hlt" << frg::endlog;
	}else if(!haveMwait) {
		debugLogger() << "thor: CPU does not support mwait" << frg::endlog;
	}else if(!haveMwaitSubStates) {
		debugLogger() << "thor: CPU does not enumerate mwait sub-states" << frg::endlog;
	}else if(frg::string_view{vendor} == "GenuineIntel") {
		mwaitStates = initializeIntelMwaitStates();
	}else{
		debugLogger() << "thor: mwait C-states are not supported for vendor " << vendor
				<< frg::endlog;
	}
	if(!mwaitStates.size()) {
		debugLogger() << "thor: Using hlt to idle" << frg::endlog;
		addState(hltIdleState);
		return;
	}

	// EDX stores the number of sub-states for each C-state (4 bits each).
	// Note that this numbering only matches Intel's naming scheme for C0 and C1
	// (i.e., mwait C-states are numbered differently than CPU C-states).
	auto mwaitLeaf = common::x86::cpuid(0x05);
	auto subStates = [&] (unsigned int index) -> uint32_t {
		if(index >= 8)
			return 0;
		return (mwaitLeaf[3] >> (4 * index)) & 0xF;
	};
	bool haveArat = vendorLeaf[0] >= 6 && (common::x86::cpuid(0x06)[0] & (uint32_t(1) << 2));
	{
		auto logger = debugLogger();
		logger << "thor: CPU supports mwait, sub-states: [";
		for(unsigned int i = 0; i < 8; ++i)
			logger << (i ? " " : "") << subStates(i);
		logger << "]";
		logger << (haveArat ? ", APIC timer always runs" : ", APIC timer stops in deep C-states");
		logger << frg::endlog;
	}

	// Deeper states stop the local APIC timer (unless the CPU has ARAT) and the TSC (unless it is invariant).
	bool timerSurvives = haveArat
			&& getGlobalCpuFeatures()->haveInvariantTsc
			&& getGlobalCpuFeatures()->haveTscDeadline;

	for(auto &mwaitState : mwaitStates) {
		// The upper 4 bits of the mwait hint select the C-state (minus one).
		// Like Linux, we only require that CPUID enumerates some sub-state of that C-state.
		if(!subStates((mwaitState.hint >> 4) + 1))
			continue;
		// Do not use states deeper than C1 if the timer does not survive in them.
		if((mwaitState.hint >> 4) && !timerSurvives)
			continue;
		addState({
			.name = mwaitState.name,
			.exitLatency = mwaitState.exitLatency,
			.targetResidency = mwaitState.targetResidency,
			.losesTlb = mwaitState.losesTlb,
			.method = {.instruction = IdleInstruction::mwait, .mwaitHint = mwaitState.hint}
		});
		debugLogger() << "thor: Using " << mwaitState.name
				<< " (mwait hint 0x" << frg::hex_fmt{mwaitState.hint} << ") to idle" << frg::endlog;
	}

	if(!data->numStates) {
		debugLogger() << "thor: CPUID does not enumerate any of the mwait C-states, using hlt to idle"
				<< frg::endlog;
		addState(hltIdleState);
	}
}

frg::span<const IdleState> getIdleStates() {
	auto *data = &idleCpuData.get();
	// The idle task does not run before initializeIdleStates().
	assert(data->numStates);
	return {data->states, data->numStates};
}

void idleUntilInterrupt(const IdleMethod &method) {
	assert(!intsAreEnabled());

	if(method.instruction == IdleInstruction::hlt) {
		// sti only takes effect after the next instruction, so no interrupt can be taken before hlt.
		asm volatile (
			"sti\n"
			"\thlt\n"
			"\tcli"
			::: "memory"
		);
		return;
	}

	// An interrupt that becomes pending after monitor makes mwait fall through.
	auto *word = &idleCpuData.get().monitorWord;
	uint32_t zero = 0;
	asm volatile ("monitor" :: "a"(word), "c"(zero), "d"(zero) : "memory");
	// As above, sti only takes effect after mwait.
	asm volatile (
		"sti\n"
		"\tmwait\n"
		"\tcli"
		:: "a"(method.mwaitHint), "c"(zero) : "memory"
	);
}

} // namespace thor
