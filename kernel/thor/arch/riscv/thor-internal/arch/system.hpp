#pragma once

#include <eir/interface.hpp>
#include <thor-internal/elf-notes.hpp>

namespace thor {

extern ManagarmElfNote<RiscvConfig> riscvConfigNote;
extern ManagarmElfNote<RiscvHartCaps> riscvHartCapsNote;

void initializeArchitecture();

} // namespace thor
