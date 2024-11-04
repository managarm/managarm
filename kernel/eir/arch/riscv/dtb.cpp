#include <eir-internal/dtb/discovery.hpp>
#include <eir-internal/main.hpp>

// Set by assembly stub.
void *eirDtbPtr;

namespace eir {

static initgraph::Task discoverMemory{
    &globalInitEngine,
    "riscv.discover-memory",
    // initgraph::Requires{acpi::getTablesDiscoveredStage()},
    // initgraph::Entails{getBus0AvailableStage()},
    [] { discoverMemoryFromDtb(eirDtbPtr); }
};

} // namespace eir
