#include <eir-internal/acpi/acpi.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

namespace eir::acpi {

namespace {

initgraph::Task detectCpusFromMadt{
    &globalInitEngine,
    "acpi.detect-cpu-count",
    initgraph::Requires{getTablesAvailableStage()},
    initgraph::Entails{getKernelLoadableStage()},
    [] {
	    if (!haveTables())
		    return;

	    uacpi_table madtTbl;
	    if (uacpi_table_find_by_signature("APIC", &madtTbl) != UACPI_STATUS_OK) {
		    infoLogger() << "eir: No MADT found" << frg::endlog;
		    return;
	    }
	    auto *madt = madtTbl.hdr;

	    size_t cpuCount = 0;
	    size_t offset = sizeof(acpi_madt);
	    while (offset < madt->length) {
		    acpi_entry_hdr generic;
		    auto genericPtr = reinterpret_cast<void *>(madtTbl.virt_addr + offset);
		    memcpy(&generic, genericPtr, sizeof(generic));

		    switch (generic.type) {
#if defined(__i386__) || defined(__x86_64__)
			    case ACPI_MADT_ENTRY_TYPE_LAPIC: {
				    acpi_madt_lapic entry;
				    memcpy(&entry, genericPtr, sizeof(entry));
				    if (entry.flags & ACPI_PIC_ENABLED)
					    ++cpuCount;
			    } break;
			    case ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC: {
				    acpi_madt_x2apic entry;
				    memcpy(&entry, genericPtr, sizeof(entry));
				    if (entry.flags & ACPI_PIC_ENABLED)
					    ++cpuCount;
			    } break;
#endif
#ifdef __riscv
			    case ACPI_MADT_ENTRY_TYPE_RINTC: {
				    acpi_madt_rintc entry;
				    memcpy(&entry, genericPtr, sizeof(entry));
				    if (entry.flags & ACPI_PIC_ENABLED)
					    ++cpuCount;
			    } break;
#endif
			    default:
				    // Do nothing.
		    }
		    offset += generic.length;
	    }

	    if (cpuCount > 0) {
		    cpuConfig.totalCpus = cpuCount;
		    infoLogger() << "eir: Detected " << cpuCount << " CPUs from MADT" << frg::endlog;
	    } else {
		    panicLogger() << "eir: Failed to detect CPUs from MADT" << frg::endlog;
	    }
    }
};

} // namespace

} // namespace eir::acpi
