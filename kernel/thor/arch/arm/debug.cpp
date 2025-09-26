#include <arch/mem_space.hpp>
#include <arch/register.hpp>
#include <eir/interface.hpp>
#include <thor-internal/arch/debug.hpp>
#include <thor-internal/elf-notes.hpp>
#include <uart/uart.hpp>

namespace thor {

constinit common::uart::AnyUart bootUart;
constinit UartLogHandler uartLogHandler{&bootUart};

extern bool debugToSerial;

extern ManagarmElfNote<BootUartConfig> bootUartConfig;
THOR_DEFINE_ELF_NOTE(bootUartConfig){elf_note_type::bootUartConfig, {}};

void setupDebugging() {
	if (debugToSerial) {
		if (bootUartConfig->type == BootUartType::pl011) {
			bootUart.emplace<common::uart::PL011>(bootUartConfig->window, 0);
		} else if (bootUartConfig->type == BootUartType::samsung) {
			bootUart.emplace<common::uart::Samsung>(bootUartConfig->window);
		}

		if (!std::holds_alternative<std::monostate>(bootUart)) {
			enableLogHandler(&uartLogHandler);
		}
	}
}

void UartLogHandler::emit(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	print(msg);
}

void UartLogHandler::emitUrgent(frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);
	print("URGENT: ", false);
	print(msg);
}

void UartLogHandler::print(frg::string_view line, bool printNL) {
	std::visit(
	    frg::overloaded{
	        [](std::monostate) { __builtin_trap(); },
	        [&](auto &inner) {
		        for (size_t i = 0; i < line.size(); ++i) {
			        auto c = line[i];
			        if (c == '\n') {
				        inner.write('\r');
				        inner.write('\n');
			        } else {
				        inner.write(c);
			        }
		        }
		        if (printNL) {
			        inner.write('\r');
			        inner.write('\n');
		        }
	        }
	    },
	    *uart_
	);
}

} // namespace thor
