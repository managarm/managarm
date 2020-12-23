#include <algorithm>
#include <hw.frigg_pb.hpp>
#include <mbus.frigg_pb.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/pci/pci.hpp>
#include <thor-internal/pci/pci_legacy.hpp>
#include <thor-internal/pci/pcie_ecam.hpp>
#include <thor-internal/main.hpp>

#include <lai/core.h>
#include <lai/helpers/pci.h>

namespace thor::pci {

struct AcpiPciIrqRouter : PciIrqRouter {
	AcpiPciIrqRouter(PciIrqRouter *parent_, PciBus *associatedBus_, lai_nsnode_t *handle);

	PciIrqRouter *makeDownstreamRouter(PciBus *bus) override;

private:
	lai_nsnode_t *acpiHandle = nullptr;
};

AcpiPciIrqRouter::AcpiPciIrqRouter(PciIrqRouter *parent_, PciBus *associatedBus_,
		lai_nsnode_t *handle)
: PciIrqRouter{parent_, associatedBus_}, acpiHandle{handle} {
	LAI_CLEANUP_STATE lai_state_t laiState;
	lai_init_state(&laiState);

	if (!acpiHandle) {
		for(int i = 0; i < 4; i++) {
			bridgeIrqs[i] = parent->resolveIrqRoute(
					associatedBus->associatedBridge->slot, static_cast<IrqIndex>(i + 1));
			if(bridgeIrqs[i])
				infoLogger() << "thor:     Bridge IRQ [" << i << "]: "
						<< bridgeIrqs[i]->name() << frg::endlog;
		}

		routingModel = RoutingModel::expansionBridge;
		return;
	}

	// Look for a PRT and evaluate it.
	lai_nsnode_t *prtHandle = lai_resolve_path(acpiHandle, "_PRT");
	if(!prtHandle) {
		if (parent) {
			infoLogger() << "thor: There is no _PRT for bus " << associatedBus->busId << ";"
					" assuming expansion bridge routing" << frg::endlog;
			for(int i = 0; i < 4; i++) {
				bridgeIrqs[i] = parent->resolveIrqRoute(
						associatedBus->associatedBridge->slot, static_cast<IrqIndex>(i + 1));
				if(bridgeIrqs[i])
					infoLogger() << "thor:     Bridge IRQ [" << i << "]: "
							<< bridgeIrqs[i]->name() << frg::endlog;
			}

			routingModel = RoutingModel::expansionBridge;
		}else{
			infoLogger() << "thor: There is no _PRT for bus " << associatedBus->busId << ";"
					" giving up IRQ routing of this bus" << frg::endlog;
		}
		return;
	}

	LAI_CLEANUP_VAR lai_variable_t prt = LAI_VAR_INITIALIZER;
	if (lai_eval(&prt, prtHandle, &laiState)) {
		infoLogger() << "thor: Failed to evaluate _PRT;"
				" giving up IRQ routing of this bus" << frg::endlog;
		return;
	}

	// Walk through the PRT and determine the routing.
	struct lai_prt_iterator iter = LAI_PRT_ITERATOR_INITIALIZER(&prt);
	lai_api_error_t e;
	while (!(e = lai_pci_parse_prt(&iter))) {
		assert(iter.function == -1 && "TODO: support routing of individual functions");
		auto index = static_cast<IrqIndex>(iter.pin + 1);

		infoLogger() << "    Route for slot " << iter.slot
				<< ", " << nameOf(index) << ": "
				<< "GSI " << iter.gsi << frg::endlog;

		// In contrast to the previous ACPICA code, LAI can resolve _CRS automatically.
		// Hence, for now we do not deal with link devices.
		configureIrq(GlobalIrqInfo{iter.gsi, {
				iter.level_triggered ? TriggerMode::level : TriggerMode::edge,
				iter.active_low ? Polarity::low : Polarity::high}});
		auto pin = getGlobalSystemIrq(iter.gsi);
		routingTable.push({static_cast<unsigned int>(iter.slot), index, pin});
	}

	routingModel = RoutingModel::rootTable;
}

PciIrqRouter *AcpiPciIrqRouter::makeDownstreamRouter(PciBus *bus) {
	lai_nsnode_t *deviceHandle = nullptr;
	if (acpiHandle) {
		LAI_CLEANUP_STATE lai_state_t laiState;
		lai_init_state(&laiState);
		deviceHandle = lai_pci_find_device(acpiHandle, bus->associatedBridge->slot,
				bus->associatedBridge->function, &laiState);
	}

	if (deviceHandle) {
		LAI_CLEANUP_FREE_STRING char *acpiPath = lai_stringify_node_path(deviceHandle);
		infoLogger() << "            ACPI: " << const_cast<const char *>(acpiPath)
				<< frg::endlog;
	}

	return frg::construct<AcpiPciIrqRouter>(*kernelAlloc, this, bus, deviceHandle);
}

static void addLegacyConfigIo() {
	auto io = frg::construct<LegacyPciConfigIo>(*kernelAlloc);
	for (int i = 0; i < 256; i++) {
		addConfigSpaceIo(0, i, io);
	}
}

struct [[gnu::packed]] McfgEntry {
	uint64_t mmioBase;
	uint16_t segment;
	uint8_t busStart;
	uint8_t busEnd;
	uint32_t reserved;
};

static initgraph::Task discoverConfigIoSpaces{&basicInitEngine, "pci.discover-acpi-config-io",
	[] {
		void *mcfgWindow = laihost_scan("MCFG", 0);
		if(!mcfgWindow) {
			infoLogger() << "\e[31m" "thor: No MCFG table!" "\e[39m" << frg::endlog;
			addLegacyConfigIo();
			return;
		}

		auto mcfg = reinterpret_cast<acpi_header_t *>(mcfgWindow);
		if(mcfg->length < sizeof(acpi_header_t) + 8 + sizeof(McfgEntry)) {
			infoLogger() << "\e[31m" "thor: MCFG table has no entries, assuming legacy PCI!" "\e[39m"
					<< frg::endlog;
			addLegacyConfigIo();
			return;
		}

		size_t nEntries = (mcfg->length - 44) / 16;
		auto mcfgEntries = (McfgEntry *)((uintptr_t)mcfgWindow + sizeof(acpi_header_t) + 8);
		for (size_t i = 0; i < nEntries; i++) {
			auto &entry = mcfgEntries[i];
			infoLogger() << "Found config space for segment " << entry.segment
				<< ", buses " << entry.busStart << "-" << entry.busEnd
				<< ", ECAM MMIO base at " << (void *)entry.mmioBase << frg::endlog;

			auto io = frg::construct<EcamPcieConfigIo>(*kernelAlloc,
					entry.mmioBase, entry.segment,
					entry.busStart, entry.busEnd);

			for (int j = entry.busStart; j <= entry.busEnd; j++) {
				addConfigSpaceIo(entry.segment, j, io);
			}
		}
	}
};


static initgraph::Task discoverAcpiRootBuses{&extendedInitEngine, "pci.discover-acpi-root-buses",
	[] {
		LAI_CLEANUP_STATE lai_state_t laiState;
		lai_init_state(&laiState);

		LAI_CLEANUP_VAR lai_variable_t pci_pnp_id = LAI_VAR_INITIALIZER;
		LAI_CLEANUP_VAR lai_variable_t pcie_pnp_id = LAI_VAR_INITIALIZER;
		lai_eisaid(&pci_pnp_id, "PNP0A03");
		lai_eisaid(&pcie_pnp_id, "PNP0A08");

		lai_nsnode_t *sb_handle = lai_resolve_path(NULL, "\\_SB_");
		LAI_ENSURE(sb_handle);
		struct lai_ns_child_iterator iter = LAI_NS_CHILD_ITERATOR_INITIALIZER(sb_handle);
		lai_nsnode_t *handle;
		while ((handle = lai_ns_child_iterate(&iter))) {
			if (lai_check_device_pnp_id(handle, &pci_pnp_id, &laiState)
					&& lai_check_device_pnp_id(handle, &pcie_pnp_id, &laiState))
				continue;

			// TODO: parse _SEG and _BUS to get the segment and bus numbers

			infoLogger() << "thor: Found PCI host bridge" << frg::endlog;
			auto rootBus = frg::construct<PciBus>(*kernelAlloc, nullptr, nullptr, getConfigIoFor(0, 0), 0, 0);
			rootBus->irqRouter = frg::construct<AcpiPciIrqRouter>(*kernelAlloc, nullptr, rootBus, handle);
			addRootBus(rootBus);
		}

		infoLogger() << "thor: Discovering PCI devices" << frg::endlog;
		enumerateAll();
	}
};

} // namespace thor::pci
