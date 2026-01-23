#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/elf-notes.hpp>

namespace thor {

THOR_DEFINE_ELF_NOTE(cpuConfigNote){elf_note_type::cpuConfig, {0}};

ExecutorContext::ExecutorContext() { }

CpuData::CpuData()
: activeFiber{nullptr}, heartbeat{0} {
	iseqPtr = &regularIseq;
}

// Define a ELF note so that eir can find the per-CPU region and map
// the KASAN shadow for it.

// HACK: We define a different struct than eir's PerCpuRegion because
// `reinterpret_cast<uint64_t>(&symbol)` is not constinit. We can't
// just use `void *` in the eir struct because eir might be a 32-bit
// binary. The two structs should have the same layout though, since
// thor is always 64-bit.

// Keep in sync with <eir/interface.hpp>!
struct OurPerCpuRegion {
	void *perCpuStart;
	void *perCpuEnd;
};

extern ManagarmElfNote<OurPerCpuRegion> perCpuRegionNote;
THOR_DEFINE_ELF_NOTE(perCpuRegionNote){elf_note_type::perCpuRegion, {&percpuStart, &percpuEnd}};

// An instance of CpuData is the first thing in every CPU's per-CPU
// region, hence it goes into a special section.
THOR_DEFINE_PERCPU_UNINITIALIZED_PRIV(cpuData, "_head");


extern "C" PerCpuInitializer percpuInitStart[], percpuInitEnd[];

namespace {

void initializePerCpuDataFor(CpuData *context) {
	for(PerCpuInitializer *p = percpuInitStart; p != percpuInitEnd; ++p) {
		(*p)(context);
	}
}

} // namespace anonymous

void runCpuDataInitializers() {
	for (size_t cpu = 0; cpu < getCpuCount(); ++cpu) {
		initializePerCpuDataFor(getCpuData(cpu));
	}
}

size_t getCpuCount() {
	return cpuConfigNote->totalCpus;
}

} // namespace thor
