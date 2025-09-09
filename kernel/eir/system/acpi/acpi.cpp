#include <eir-internal/acpi/acpi.hpp>
#include <eir-internal/main.hpp>
#include <uacpi/uacpi.h>

namespace eir::acpi {

namespace {

constinit std::array<uint8_t, 4096> earlyTableBuffer{};

initgraph::Task setupTables{
    &globalInitEngine, "acpi.setup-tables", initgraph::Requires{getRsdpAvailableStage()}, [] {
	    if (!eirRsdpAddr) {
		    infoLogger() << "eir: No RSDP available, skipping ACPI table setup" << frg::endlog;
		    return;
	    }

	    checkOrPanic(
	        uacpi_setup_early_table_access(earlyTableBuffer.data(), earlyTableBuffer.size())
	    );
    }
};

} // anonymous namespace.

initgraph::Stage *getRsdpAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "acpi.rsdp-available"};
	return &s;
}

} // namespace eir::acpi
