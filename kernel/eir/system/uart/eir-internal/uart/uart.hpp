#pragma once

#include <span>

#include <dtb.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/main.hpp>
#include <uacpi/acpi.h>
#include <uart/uart.hpp>

namespace eir::uart {

struct UartLogHandler : LogHandler {
	UartLogHandler(common::uart::AnyUart *);

	UartLogHandler(const UartLogHandler &) = delete;
	UartLogHandler &operator=(const UartLogHandler &) = delete;

	void emit(frg::string_view line) override;

private:
	common::uart::AnyUart *uart_;
};

// This is consumed by Eir's generic code to fill the boot UART tag.
extern BootUartConfig bootUartConfig;

void setBootUart(common::uart::AnyUart *uartPtr);

// Initialize a UART from the DBG2 or SPCR tables.
// For DBG2, the type (not subtype) must be serial (= 0x8000).
// The subtype that is passed to this function is also defined by DBG2.
void initFromAcpi(common::uart::AnyUart &uart, unsigned int subtype, const acpi_gas &base);

void initFromDtb(common::uart::AnyUart &uart, std::span<DeviceTreeNode> path);

// The boot UART must be determined before this stage.
initgraph::Stage *getBootUartDeterminedStage();

} // namespace eir::uart
