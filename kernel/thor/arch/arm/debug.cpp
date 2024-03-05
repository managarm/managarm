#include <thor-internal/arch/debug.hpp>
#include <arch/aarch64/mem_space.hpp>
#include <arch/register.hpp>

namespace thor {

constinit UartLogHandler uartLogHandler;

extern bool debugToSerial;

void setupDebugging() {
	if (debugToSerial)
		enableLogHandler(&uartLogHandler);
}

namespace {
	namespace reg {
		static constexpr arch::scalar_register<uint32_t> data{0x00};
		static constexpr arch::bit_register<uint32_t> status{0x18};
	}

	namespace status {
		static constexpr arch::field<uint32_t, bool> tx_full{5, 1};
	};

	static constexpr arch::mem_space space{0xFFFF000000000000};

} // namespace anonymous

void UartLogHandler::printChar(char c) {
	// Here we depend on a few things:
	// 1. Eir has mapped the UART to 0xFFFF000000000000
	// 2. The UART is at least somewhat PL011 compatible
	// 3. The UART is already configured by Eir to some sensible settings

	while (space.load(reg::status) & status::tx_full)
		;

	space.store(reg::data, c);
}

} // namespace thor
