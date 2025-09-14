#pragma once

#include <source_location>

#include <eir-internal/debug.hpp>
#include <initgraph.hpp>
#include <uacpi/status.h>

namespace eir::acpi {

inline void
checkOrPanic(uacpi_status status, std::source_location srcloc = std::source_location::current()) {
	if (status != UACPI_STATUS_OK)
		panicLogger() << "uACPI failure: " << uacpi_status_to_string(status) << " in function "
		              << srcloc.function_name() << " at " << srcloc.file_name() << ":"
		              << srcloc.line() << frg::endlog;
};

// Returns true if the system has ACPI tables.
// False if the system does not use ACPI or if ACPI tables are disabled or faulty.
// Only valid after getTablesAvailableStage().
bool haveTables();

// eirRsdpAddr is available at this stage.
initgraph::Stage *getRsdpAvailableStage();

// uACPI can be used to retrieve ACPI tables at this stage.
initgraph::Stage *getTablesAvailableStage();

} // namespace eir::acpi
