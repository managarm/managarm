#pragma once

#include <thor-internal/irq.hpp>
#include <uacpi/uacpi.h>
#include <initgraph.hpp>

namespace thor {

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

namespace thor {
namespace acpi {

initgraph::Stage *getTablesDiscoveredStage();
initgraph::Stage *getNsAvailableStage();

void initGlue();

inline uacpi_object_name makeSignature(const char(&sig)[5]) {
	return {{sig[0], sig[1], sig[2], sig[3]}};
}

} } // namespace thor::acpi
