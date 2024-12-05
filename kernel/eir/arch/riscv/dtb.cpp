#include <eir-internal/dtb/discovery.hpp>
#include <eir-internal/main.hpp>

namespace eir {

static initgraph::Task discoverMemory{
    &globalInitEngine, "riscv.discover-memory", initgraph::Entails{getInitrdAvailableStage()}, [] {
	    discoverMemoryFromDtb(eirDtbPtr);
    }
};

} // namespace eir
