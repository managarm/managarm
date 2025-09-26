#pragma once

#include <variant>

#include <arch/io_space.hpp>
#include <arch/mem_space.hpp>
#include <uart/ns16550.hpp>
#include <uart/pl011.hpp>
#include <uart/samsung.hpp>

namespace common::uart {

using AnyUart =
    std::variant<std::monostate, Ns16550<arch::mem_space>, Ns16550<arch::io_space>, PL011, Samsung>;

} // namespace common::uart
