#pragma once

#include <thor-internal/irq.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/mbus.hpp>
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

namespace thor::acpi {

extern KernelFiber *acpiFiber;

constexpr const uacpi_char *ACPI_HID_PCI = "PNP0A03";
constexpr const uacpi_char *ACPI_HID_PCIE = "PNP0A08";
constexpr const uacpi_char *ACPI_HID_EC = "PNP0C09";
constexpr const uacpi_char *ACPI_HID_BATTERY = "PNP0C0A";
constexpr const uacpi_char *ACPI_HID_POWER_BUTTON = "PNP0C0C";

initgraph::Stage *getTablesDiscoveredStage();
initgraph::Stage *getNsAvailableStage();
initgraph::Stage *getAcpiWorkqueueAvailableStage();

void initGlue();
void initEc();
void initEvents();

struct AcpiObject final : public KernelBusObject {
	AcpiObject(uacpi_namespace_node *node, const char *hid, unsigned int id)
	: node{node}, hid_name{hid}, instance{id} {}

	coroutine<void> run();
	coroutine<frg::expected<Error>> handleRequest(LaneHandle lane) override;

	size_t mbus_id;
	uacpi_namespace_node *node;
	const char *hid_name;
	unsigned int instance;
};

} // namespace thor::acpi
