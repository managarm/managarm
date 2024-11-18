#include <eir-internal/main.hpp>
#include <eir-internal/dtb/discovery.hpp>

namespace eir {

static initgraph::Task discoverMemory{&globalInitEngine, "riscv.discover-memory",
	[] {
		discoverMemoryFromDtb(eirDtbPtr);
	}
};

} // namespace eir
