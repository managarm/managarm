#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>
#include <eir/interface.hpp>
#include <frg/manual_box.hpp>

namespace eir {

static BootCaps limineCaps = {
    .hasMemoryMap = false,
};

BootCaps *BootCaps::get() { return &limineCaps; };

} // namespace eir
