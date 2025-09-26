#include <eir-internal/acpi/acpi.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/uart/uart.hpp>
#include <frg/manual_box.hpp>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

namespace eir::acpi {

namespace {

constinit common::uart::AnyUart acpiUart;
constinit frg::manual_box<uart::UartLogHandler> acpiUartLogHandler;

initgraph::Task parseSpcrDbg2{
    &globalInitEngine,
    "acpi.parse-spcr-dbg2",
    initgraph::Requires{getTablesAvailableStage()},
    initgraph::Entails{uart::getBootUartDeterminedStage()},
    [] {
	    if (!haveTables())
		    return;

	    uacpi_table spcrTbl;
	    uacpi_table dbg2Tbl;
	    bool haveSpcr = false;
	    bool haveDbg2 = false;
	    if (auto status = uacpi_table_find_by_signature("SPCR", &spcrTbl);
	        status == UACPI_STATUS_OK) {
		    haveSpcr = true;
	    }
	    if (auto status = uacpi_table_find_by_signature("DBG2", &dbg2Tbl);
	        status == UACPI_STATUS_OK) {
		    haveDbg2 = true;
	    }

	    // We prefer SPCR to DBG2.
	    // SPCR specifies the UART that we should launch a console on,
	    // while DBG2 lists the available debug ports.
	    // However, in reality, there is no clear distinction between the tables
	    // and the entries in both tables are often identical.

	    if (haveSpcr) {
		    auto spcr = static_cast<acpi_spcr *>(spcrTbl.ptr);
		    infoLogger() << "eir: SPCR UART subtype " << spcr->interface_type
		                 << ", address space: 0x"
		                 << frg::hex_fmt{spcr->base_address.address_space_id} << ", base: 0x"
		                 << frg::hex_fmt{spcr->base_address.address} << frg::endlog;

		    uart::initFromAcpi(acpiUart, spcr->interface_type, spcr->base_address);

		    if (!std::holds_alternative<std::monostate>(acpiUart)) {
			    acpiUartLogHandler.initialize(&acpiUart);
			    enableLogHandler(acpiUartLogHandler.get());
			    uart::setBootUart(&acpiUart);
			    return;
		    }
	    }

	    if (haveDbg2) {
		    auto dbg2 = static_cast<acpi_dbg2 *>(dbg2Tbl.ptr);
		    auto nextInfo = reinterpret_cast<char *>(dbg2Tbl.ptr) + dbg2->offset_dbg_device_info;
		    for (size_t i = 0; i < dbg2->number_dbg_device_info; ++i) {
			    auto info = reinterpret_cast<acpi_dbg2_dbg_device_info *>(nextInfo);
			    nextInfo += info->length;

			    if (info->port_type != ACPI_DBG2_TYPE_SERIAL) {
				    infoLogger() << "eir: DBG2 port type 0x" << frg::hex_fmt{info->port_type}
				                 << " is not supported" << frg::endlog;
				    continue;
			    }
			    if (info->number_generic_address_registers != 1) {
				    infoLogger()
				        << "eir: DBG2 UARTs with more than one register base are not supported"
				        << frg::endlog;
				    continue;
			    }

			    auto gas = reinterpret_cast<acpi_gas *>(
			        reinterpret_cast<char *>(info) + info->base_address_register_offset
			    );

			    infoLogger() << "eir: DBG2 UART subtype " << info->port_subtype
			                 << ", address space: 0x" << frg::hex_fmt{gas->address_space_id}
			                 << ", base: 0x" << frg::hex_fmt{gas->address} << frg::endlog;

			    uart::initFromAcpi(acpiUart, info->port_subtype, *gas);

			    if (!std::holds_alternative<std::monostate>(acpiUart)) {
				    acpiUartLogHandler.initialize(&acpiUart);
				    enableLogHandler(acpiUartLogHandler.get());
				    uart::setBootUart(&acpiUart);
				    return;
			    }
		    }
	    }
    }
};

} // anonymous namespace.

} // namespace eir::acpi
