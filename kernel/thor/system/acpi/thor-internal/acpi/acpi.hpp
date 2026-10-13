#pragma once

#include <eir/interface.hpp>
#include <initgraph.hpp>
#include <smarter.hpp>
#include <thor-internal/elf-notes.hpp>
#include <thor-internal/irq.hpp>
#include <uacpi/uacpi.h>

namespace thor {

extern ManagarmElfNote<AcpiData> acpiRsdpNote;

// Stores the global IRQ information (GSI, trigger mode, polarity)
// (in constrast to bus-specific information, e.g., for IRQs on the ISA bus).
struct GlobalIrqInfo {
	unsigned int gsi;
	IrqConfiguration configuration;
};

GlobalIrqInfo resolveIsaIrq(unsigned int irq);
GlobalIrqInfo resolveIsaIrq(unsigned int irq, IrqConfiguration desired);
void configureIrq(GlobalIrqInfo info);
} // namespace thor

namespace thor::acpi {

smarter::shared_ptr<IrqPin> getGlobalSystemIrq(size_t n);
void setGlobalSystemIrq(size_t n, smarter::shared_ptr<IrqPin> pin);

initgraph::Stage *getTablesDiscoveredStage();

// Applies the uacpi.log kernel command line option.
void configureLogLevel();

void initGlue();

} // namespace thor::acpi
