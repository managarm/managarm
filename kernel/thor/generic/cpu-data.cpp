#include <thor-internal/cpu-data.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/elf-notes.hpp>

namespace thor {

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

constinit void *curPos = percpuEnd;
constinit size_t numExtraCpus = 0;


void initializePerCpuDataFor(CpuData *context) {
	for(PerCpuInitializer *p = percpuInitStart; p != percpuInitEnd; ++p) {
		(*p)(context);
	}
}

} // namespace anonymous

void runBootCpuDataInitializers() {
	initializePerCpuDataFor(reinterpret_cast<CpuData *>(percpuStart));
}

std::tuple<CpuData *, size_t> extendPerCpuData() {
	size_t size = percpuEnd - percpuStart;
	assert(!(size & 0xFFF));

	auto base = reinterpret_cast<uintptr_t>(curPos);

	// Make sure we don't wrap around.
	assert((base + size) > reinterpret_cast<uintptr_t>(percpuStart));
	// TODO(qookie): These two lines need to be protected with a
	// lock if we want to start CPUs in parallel.
	curPos = reinterpret_cast<void *>(base + size);
	int cpuNr = ++numExtraCpus;

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto page = physicalAllocator->allocate(kPageSize);
		assert(page != PhysicalAddr(-1) && "OOM");

		KernelPageSpace::global().mapSingle4k(base + pg, page, page_access::write, CachingMode::null);
	}
	unpoisonKasanShadow(reinterpret_cast<void *>(base), size);

	auto context = reinterpret_cast<CpuData *>(base);
	initializePerCpuDataFor(context);

	return {context, cpuNr};
}

size_t getCpuCount() {
	return numExtraCpus + 1;
}

} // namespace thor
