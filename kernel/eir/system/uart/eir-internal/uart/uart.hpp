#pragma once

#include <variant>

#include <dtb.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/uart/ns16550.hpp>
#include <eir-internal/uart/pl011.hpp>
#include <uacpi/acpi.h>

namespace eir::uart {

using AnyUart =
    std::variant<std::monostate, Ns16550<arch::mem_space>, Ns16550<arch::io_space>, PL011>;

struct UartLogHandler : LogHandler {
	UartLogHandler(AnyUart *);

	UartLogHandler(const UartLogHandler &) = delete;
	UartLogHandler &operator=(const UartLogHandler &) = delete;

	void emit(frg::string_view line) override;

private:
	AnyUart *uart_;
};

// Initialize a UART from the DBG2 or SPCR tables.
// For DBG2, the type (not subtype) must be serial (= 0x8000).
// The subtype that is passed to this function is also defined by DBG2.
void initFromAcpi(AnyUart &uart, unsigned int subtype, const acpi_gas &base);

void initFromDtb(AnyUart &uart, const DeviceTree &tree, const DeviceTreeNode &node);

} // namespace eir::uart
