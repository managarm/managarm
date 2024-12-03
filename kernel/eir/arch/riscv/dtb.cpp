#include <eir-internal/dtb/discovery.hpp>
#include <eir-internal/main.hpp>

namespace eir {

static initgraph::Task discoverMemory{&globalInitEngine, "riscv.discover-memory", [] {
	                                      discoverMemoryFromDtb(eirDtbPtr);
                                      }};

} // namespace eir
