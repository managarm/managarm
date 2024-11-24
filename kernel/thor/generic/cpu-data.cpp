#include <thor-internal/cpu-data.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/arch-generic/paging.hpp>

namespace thor {

extern "C" PerCpuInitializer percpuInitStart[], percpuInitEnd[];

namespace {

constinit void *curPos = percpuEnd;
constinit size_t numCpus = 0;


void initializePerCpuDataFor(void *context) {
	uintptr_t base = reinterpret_cast<uintptr_t>(context);

	for(PerCpuInitializer *p = percpuInitStart; p != percpuInitEnd; ++p) {
		auto offset =
			reinterpret_cast<uintptr_t>(p->target0)
			- reinterpret_cast<uintptr_t>(percpuStart);

		p->init(reinterpret_cast<void *>(base + offset));
	}
}

} // namespace anonymous

void runBootCpuDataInitializers() {
	initializePerCpuDataFor(percpuStart);
}

void *addNewPerCpuData() {
	size_t size = percpuEnd - percpuStart;
	assert(!(size & 0xFFF));

	auto base = reinterpret_cast<uintptr_t>(curPos);

	// Make sure we don't wrap around.
	assert((base + size) > reinterpret_cast<uintptr_t>(percpuStart));
	// TODO(qookie): This needs to be locked if we want to start
	// CPUs in parallel.
	curPos = reinterpret_cast<void *>(base + size);
	numCpus++;

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto page = physicalAllocator->allocate(kPageSize);
		assert(page != PhysicalAddr(-1) && "OOM");

		KernelPageSpace::global().mapSingle4k(base + pg, page, page_access::write, CachingMode::null);
	}

	auto context = reinterpret_cast<void *>(base);
	initializePerCpuDataFor(context);

	return context;
}

size_t getCpuCount() {
	return numCpus;
}

} // namespace thor
