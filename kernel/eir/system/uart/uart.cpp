#include <eir-internal/uart/uart.hpp>

namespace eir::uart {

UartLogHandler::UartLogHandler(AnyUart *uart) : uart_{uart} {}

void UartLogHandler::emit(frg::string_view line) {
	std::visit(
	    [&]<typename Inner>(Inner &inner) {
		    if constexpr (std::is_same_v<Inner, std::monostate>) {
			    __builtin_trap();
		    } else {
			    for (size_t i = 0; i < line.size(); ++i) {
				    auto c = line[i];
				    if (c == '\n') {
					    inner.write('\r');
					    inner.write('\n');
				    } else {
					    inner.write(c);
				    }
			    }
			    inner.write('\r');
			    inner.write('\n');
		    }
	    },
	    *uart_
	);
}

void initFromAcpi(AnyUart &uart, unsigned int subtype, const acpi_gas &base) {
	switch (subtype) {
		case ACPI_DBG2_SUBTYPE_SERIAL_NS16550:
			[[fallthrough]];
		case ACPI_DBG2_SUBTYPE_SERIAL_NS16550_DBGP1: {
			if (base.address_space_id == ACPI_AS_ID_SYS_MEM) {
				uart.emplace<Ns16550<arch::mem_space>>(arch::global_mem.subspace(base.address));
			} else if (base.address_space_id == ACPI_AS_ID_SYS_IO) {
				uart.emplace<Ns16550<arch::io_space>>(arch::global_io.subspace(base.address));
			} else {
				infoLogger() << "eir: Unsupported ACPI address space 0x"
				             << frg::hex_fmt{base.address_space_id} << " for NS16550"
				             << frg::endlog;
			}
			break;
		}
		case ACPI_DBG2_SUBTYPE_SERIAL_PL011:
			if (base.address_space_id != ACPI_AS_ID_SYS_MEM) {
				infoLogger() << "eir: Unsupported ACPI address space 0x"
				             << frg::hex_fmt{base.address_space_id} << " for PL011" << frg::endlog;
				return;
			}
			// We assume that the PL011 is already initialized (i.e., that the baud rate is set up
			// correctly etc.). Hence, we do not need to pass a proper clock rate here.
			uart.emplace<PL011>(base.address, 0);
			break;
		default:
			infoLogger() << "eir: Unsupported ACPI UART subtype 0x" << frg::hex_fmt{subtype}
			             << frg::endlog;
	}
}

void initFromDtb(AnyUart &uart, const DeviceTree &tree, const DeviceTreeNode &chosen) {
	auto compatible = chosen.findProperty("compatible")->asString();

	if (*compatible == "arm,pl011") {
		auto addr = chosen.findProperty("reg")->asU64(0);
		uart.emplace<PL011>(addr, 0);
	}
}

} // namespace eir::uart
