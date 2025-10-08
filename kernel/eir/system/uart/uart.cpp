#include <eir-internal/arch.hpp>
#include <eir-internal/dtb/helpers.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>
#include <eir-internal/uart/uart.hpp>
#include <frg/utility.hpp>

namespace eir::uart {

namespace {

constinit common::uart::AnyUart *bootUartPtr{nullptr};

bool isMmio(BootUartType type) {
	switch (type) {
		case BootUartType::pl011:
		case BootUartType::samsung:
			return true;
		default:
			return false;
	}
}

template <typename Space>
void getBootUartConfig(common::uart::Ns16550<Space> &, BootUartConfig &) {}

void getBootUartConfig(common::uart::PL011 &uart, BootUartConfig &config) {
	config.type = BootUartType::pl011;
	config.address = uart.base();
	config.size = 0x1000;
}

void getBootUartConfig(common::uart::Samsung &uart, BootUartConfig &config) {
	config.type = BootUartType::samsung;
	config.address = uart.base();
	config.size = 0x1000;
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
	            [](auto &inner) { getBootUartConfig(inner, bootUartConfig); }
	        },
	        *bootUartPtr
	    );

	    size_t extent = bootUartConfig.size + (bootUartConfig.address & (pageSize - 1));
	    // Round up to the next multiple of page size.
	    reserveEarlyMmio(((extent + pageSize - 1) & ~(pageSize - 1)) >> pageShift);
    }
};

static initgraph::Task setupBootUartMmio{
    &globalInitEngine,
    "uart.setup-boot-uart-mmio",
    initgraph::Requires{getBootUartDeterminedStage(), getKernelMappableStage()},
    initgraph::Entails{getKernelLoadableStage()},
    [] {
	    if (!bootUartPtr)
		    return;

	    if (isMmio(bootUartConfig.type)) {
		    size_t extent = bootUartConfig.size + (bootUartConfig.address & (pageSize - 1));
		    // Round up to the next multiple of page size.
		    size_t pages = ((extent + pageSize - 1) & ~(pageSize - 1)) >> pageShift;

		    auto window = allocateEarlyMmio(pages);
		    for (size_t i = 0; i < pages; ++i) {
			    mapSingle4kPage(
			        window + i * pageSize,
			        bootUartConfig.address + i * pageSize,
			        PageFlags::write,
			        CachingMode::mmio
			    );
		    }

		    mapKasanShadow(window, pages * pageSize);
		    unpoisonKasanShadow(window, pages * pageSize);

		    bootUartConfig.window = window;
	    }
    }
};

} // anonymous namespace

BootUartConfig bootUartConfig;

UartLogHandler::UartLogHandler(common::uart::AnyUart *uart) : uart_{uart} {}

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

void setBootUart(common::uart::AnyUart *uartPtr) { bootUartPtr = uartPtr; }

void initFromAcpi(common::uart::AnyUart &uart, unsigned int subtype, const acpi_gas &base) {
	switch (subtype) {
		case ACPI_DBG2_SUBTYPE_SERIAL_NS16550:
			[[fallthrough]];
		case ACPI_DBG2_SUBTYPE_SERIAL_NS16550_DBGP1: {
			if (base.address_space_id == ACPI_AS_ID_SYS_MEM) {
				uart.emplace<common::uart::Ns16550<arch::mem_space>>(
				    arch::global_mem.subspace(base.address)
				);
			} else if (base.address_space_id == ACPI_AS_ID_SYS_IO) {
				uart.emplace<common::uart::Ns16550<arch::io_space>>(
				    arch::global_io.subspace(base.address)
				);
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
			uart.emplace<common::uart::PL011>(base.address, 0);
			break;
		default:
			infoLogger() << "eir: Unsupported ACPI UART subtype 0x" << frg::hex_fmt{subtype}
			             << frg::endlog;
	}
}

void initFromDtb(common::uart::AnyUart &uart, std::span<DeviceTreeNode> path) {
	assert(path.size() > 0);
	auto uartNode = path.back();

	auto parentPath = path.subspan(0, path.size() - 1);
	if (parentPath.empty()) {
		infoLogger() << "eir: Cannot initialize UART from DT root node" << frg::endlog;
		return;
	}
	auto parentNode = parentPath.back();
	auto addressCells = dtb::addressCells(parentNode);

	auto compatibleProperty = uartNode.findProperty("compatible");
	if (!compatibleProperty) {
		infoLogger() << "eir: No compatible string" << frg::endlog;
		return;
	}

	size_t i = 0;
	while (true) {
		auto compatibleStr = compatibleProperty->asString(i);
		if (!compatibleStr)
			break;
		++i;
		infoLogger() << *compatibleStr << frg::endlog;

		auto regProperty = uartNode.findProperty("reg");
		if (!regProperty) {
			infoLogger() << "eir: UART has no reg property" << frg::endlog;
			continue;
		}

		auto reg = regProperty->access();
		uint64_t address;
		if (!reg.readCells(address, addressCells)) {
			infoLogger() << "eir: Failed to read UART address" << frg::endlog;
			continue;
		}
		auto translated = eir::dtb::translateAddress(address, parentPath);
		if (!translated) {
			infoLogger() << "eir: Failed to translate UART address" << frg::endlog;
			continue;
		}

		if (*compatibleStr == "arm,pl011") {
			uart.emplace<common::uart::PL011>(*translated, 0);
			return;
		} else if (*compatibleStr == "apple,s5l-uart") {
			uart.emplace<common::uart::Samsung>(*translated);
			return;
		}
	}
}

initgraph::Stage *getBootUartDeterminedStage() {
	static initgraph::Stage s{&globalInitEngine, "uart.boot-uart-determined"};
	return &s;
}

} // namespace eir::uart
