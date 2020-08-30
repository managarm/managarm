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
#include <thor-internal/main.hpp>

#include <lai/core.h>
#include <lai/helpers/pci.h>

namespace thor::pci {

struct AcpiPciBus : PciBus {
	AcpiPciBus(PciBridge *associatedBridge, uint32_t busId, lai_nsnode_t *acpiHandle);

	IrqPin *resolveIrqRoute(uint32_t slot, IrqIndex index) override;
	PciBus *makeDownstreamBus(PciBridge *bridge, uint32_t busId) override;

	lai_nsnode_t *acpiHandle_ = nullptr;

private:
	RoutingModel _routingModel = RoutingModel::none;
	// PRT of this bus (RoutingModel::rootTable).
	frigg::Vector<RoutingEntry, KernelAlloc> _routingTable{*kernelAlloc};
	// IRQs of the bridge (RoutingModel::expansionBridge).
	IrqPin *_bridgeIrqs[4] = {};

};

AcpiPciBus::AcpiPciBus(PciBridge *associatedBridge, uint32_t busId, lai_nsnode_t *acpiHandle)
: PciBus{associatedBridge, busId}, acpiHandle_{acpiHandle} {
	LAI_CLEANUP_STATE lai_state_t laiState;
	lai_init_state(&laiState);

	// Look for a PRT and evaluate it.
	lai_nsnode_t *prtHandle = lai_resolve_path(acpiHandle, "_PRT");
	if(!prtHandle) {
		if(associatedBridge) {
			infoLogger() << "thor: There is no _PRT for bus " << busId << ";"
					" assuming expansion bridge routing" << frg::endlog;
			for(int i = 0; i < 4; i++) {
				_bridgeIrqs[i] = associatedBridge->parentBus->resolveIrqRoute(
						associatedBridge->slot, static_cast<IrqIndex>(i + 1));
				if(_bridgeIrqs[i])
					infoLogger() << "thor:     Bridge IRQ [" << i << "]: "
							<< _bridgeIrqs[i]->name() << frg::endlog;
			}
			_routingModel = RoutingModel::expansionBridge;
		}else{
			infoLogger() << "thor: There is no _PRT for bus " << busId << ";"
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
		_routingTable.push({static_cast<unsigned int>(iter.slot), index, pin});
	}
	_routingModel = RoutingModel::rootTable;
}

IrqPin *AcpiPciBus::resolveIrqRoute(uint32_t slot, IrqIndex index) {
	if(_routingModel == RoutingModel::rootTable) {
		auto entry = std::find_if(_routingTable.begin(), _routingTable.end(),
				[&] (const auto &ref) { return ref.slot == slot && ref.index == index; });
		if(entry == _routingTable.end())
			return nullptr;
		assert(entry->pin);
		return entry->pin;
	}else if(_routingModel == RoutingModel::expansionBridge) {
		return _bridgeIrqs[(static_cast<int>(index) - 1 + slot) % 4];
	}else{
		return nullptr;
	}
}

PciBus *AcpiPciBus::makeDownstreamBus(PciBridge *bridge, uint32_t busId) {
	lai_nsnode_t *deviceHandle = nullptr;
	if (acpiHandle_) {
		LAI_CLEANUP_STATE lai_state_t laiState;
		lai_init_state(&laiState);
		deviceHandle = lai_pci_find_device(acpiHandle_, bridge->slot, bridge->function, &laiState);
	}

	if (deviceHandle) {
		LAI_CLEANUP_FREE_STRING char *acpiPath = lai_stringify_node_path(deviceHandle);
		infoLogger() << "            ACPI: " << const_cast<const char *>(acpiPath)
				<< frg::endlog;
	}

	return frg::construct<AcpiPciBus>(*kernelAlloc, associatedBridge, busId, deviceHandle);
}

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

			infoLogger() << "thor: Found PCI host bridge" << frg::endlog;
			auto rootBus = frg::construct<AcpiPciBus>(*kernelAlloc, nullptr, 0, handle);
			addToEnumerationQueue(rootBus);
		}

		infoLogger() << "thor: Discovering PCI devices" << frg::endlog;
		enumerateAll();
	}
};

} // namespace thor::pci
