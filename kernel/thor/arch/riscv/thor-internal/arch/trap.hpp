#pragma once

#include <thor-internal/arch-generic/cpu.hpp>

namespace thor {

extern "C" void thorExceptionEntry();

void handleRiscvWorkOnExecutor(Executor *executor, Frame *frame);

} // namespace thor
