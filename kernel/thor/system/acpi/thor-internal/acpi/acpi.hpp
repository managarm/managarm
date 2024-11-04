#pragma once

#include <initgraph.hpp>
#include <smarter.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/mbus.hpp>
#include <thor-internal/stream.hpp>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

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

constexpr std::array<const uacpi_char *, 27> ACPI_HID_PS2_KEYBOARDS = {{
    "PNP0300", "PNP0301", "PNP0302", "PNP0303", "PNP0304", "PNP0305",  "PNP0306",
    "PNP0307", "PNP0308", "PNP0309", "PNP030A", "PNP030B", "PNP0320",  "PNP0321",
    "PNP0322", "PNP0323", "PNP0324", "PNP0325", "PNP0326", "PNP0327",  "PNP0340",
    "PNP0341", "PNP0342", "PNP0343", "PNP0343", "PNP0344", UACPI_NULL,
}};

constexpr std::array<const uacpi_char *, 39> ACPI_HID_PS2_MICE = {{
    "PNP0F00", "PNP0F01", "PNP0F02", "PNP0F03", "PNP0F04", "PNP0F05", "PNP0F06",  "PNP0F07",
    "PNP0F08", "PNP0F09", "PNP0F0A", "PNP0F0B", "PNP0F0C", "PNP0F0D", "PNP0F0E",  "PNP0F0F",
    "PNP0F10", "PNP0F11", "PNP0F12", "PNP0F13", "PNP0F14", "PNP0F15", "PNP0F16",  "PNP0F17",
    "PNP0F18", "PNP0F19", "PNP0F1A", "PNP0F1B", "PNP0F1C", "PNP0F1D", "PNP0F1E",  "PNP0F1F",
    "PNP0F20", "PNP0F21", "PNP0F22", "PNP0F23", "PNP0FFC", "PNP0FFF", UACPI_NULL,
}};

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
	AcpiObject(uacpi_namespace_node *node, unsigned int id) : node{node}, instance{id} {
		if (node) {
			uacpi_eval_hid(node, &hid_name);
			uacpi_eval_cid(node, &cid_name);
		}
	}

	~AcpiObject() {
		if (hid_name)
			uacpi_free_id_string(hid_name);
		if (cid_name)
			uacpi_free_pnp_id_list(cid_name);
	}

	coroutine<void> run();
	coroutine<frg::expected<Error>> handleRequest(LaneHandle lane) override;

	size_t mbus_id;
	uacpi_namespace_node *node;
	uacpi_id_string *hid_name = nullptr;
	uacpi_pnp_id_list *cid_name = nullptr;
	unsigned int instance;
	async::oneshot_event completion = {};
};

} // namespace thor::acpi
