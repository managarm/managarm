#pragma once

#include <eir/interface.hpp>
#include <thor-internal/elf-notes.hpp>

namespace thor {

static inline constexpr int numIrqSlots = 0;

extern ManagarmElfNote<RiscvConfig> riscvConfigNote;

void initializeArchitecture();

} // namespace thor
