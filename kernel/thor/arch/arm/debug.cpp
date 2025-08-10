#include <thor-internal/arch/debug.hpp>
#include <thor-internal/elf-notes.hpp>
#include <eir/interface.hpp>
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
} // namespace anonymous

extern ManagarmElfNote<BootUartConfig> bootUartConfig;
THOR_DEFINE_ELF_NOTE(bootUartConfig){elf_note_type::bootUartConfig, {}};

void UartLogHandler::emit(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	for (size_t i = 0; i < msg.size(); ++i)
		printChar(msg[i]);
	printChar('\n');
}

void UartLogHandler::emitUrgent(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	const char *prefix = "URGENT: ";
	while(*prefix)
		printChar(*(prefix++));
	for (size_t i = 0; i < msg.size(); ++i)
		printChar(msg[i]);
	printChar('\n');
}

void UartLogHandler::printChar(char c) {
	if (bootUartConfig->type != BootUartType::pl011)
		return;

	arch::mem_space space{bootUartConfig->address};

	// We assume here that Eir has mapped the UART as device memory, and
	// configured the UART to some sensible settings (115200 8N1).

	while (space.load(reg::status) & status::tx_full)
		;

	space.store(reg::data, c);
}

} // namespace thor
