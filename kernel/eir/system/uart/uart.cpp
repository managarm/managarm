#include <eir-internal/arch.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>
#include <eir-internal/uart/uart.hpp>
#include <frg/utility.hpp>

namespace eir::uart {

namespace {

constinit AnyUart *bootUartPtr{nullptr};

bool isMmio(BootUartType type) {
	switch (type) {
		case BootUartType::pl011:
			return true;
		default:
			return false;
	}
}

static initgraph::Task reserveBootUartMmio{
    &globalInitEngine,
    "uart.reserve-boot-uart-mmio",
    initgraph::Requires{getBootUartDeterminedStage()},
    initgraph::Entails{getMemoryRegionsKnownStage()},
    [] {
	    if (!bootUartPtr)
		    return;

	    std::visit(
	        frg::overloaded{
	            [](std::monostate) { __builtin_trap(); },
	            [](auto &inner) { inner.getBootUartConfig(bootUartConfig); }
	        },
	        *bootUartPtr
	    );

	    reserveEarlyMmio(1);
    }
};

static initgraph::Task setupBootUartMmio{
    &globalInitEngine,
    "uart.setup-boot-uart-mmio",
    initgraph::Requires{getBootUartDeterminedStage(), getAllocationAvailableStage()},
    initgraph::Entails{getKernelLoadableStage()},
    [] {
	    if (!bootUartPtr)
		    return;

	    if (isMmio(bootUartConfig.type)) {
		    auto window = allocateEarlyMmio(1);
		    mapSingle4kPage(window, bootUartConfig.address, PageFlags::write, CachingMode::mmio);
		    mapKasanShadow(window, 0x1000);
		    unpoisonKasanShadow(window, 0x1000);
		    bootUartConfig.window = window;
	    }
    }
};

} // anonymous namespace

BootUartConfig bootUartConfig;

UartLogHandler::UartLogHandler(AnyUart *uart) : uart_{uart} {}

void UartLogHandler::emit(frg::string_view line) {
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
		        inner.write('\r');
		        inner.write('\n');
	        }
	    },
	    *uart_
	);
}

void setBootUart(AnyUart *uartPtr) { bootUartPtr = uartPtr; }

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

void initFromDtb(AnyUart &uart, const DeviceTree &, const DeviceTreeNode &chosen) {
	auto compatible = chosen.findProperty("compatible")->asString();

	if (*compatible == "arm,pl011") {
		auto addr = chosen.findProperty("reg")->asU64(0);
		uart.emplace<PL011>(addr, 0);
	}
}

initgraph::Stage *getBootUartDeterminedStage() {
	static initgraph::Stage s{&globalInitEngine, "uart.boot-uart-determined"};
	return &s;
}

} // namespace eir::uart
