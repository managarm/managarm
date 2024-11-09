#pragma once

#include <source_location>

namespace thor {

[[noreturn]] void unimplementedOnRiscv(std::source_location loc = std::source_location::current());

} // namespace thor
