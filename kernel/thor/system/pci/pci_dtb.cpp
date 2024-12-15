#include <thor-internal/io.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/pci/pci.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/dtb/irq.hpp>
#include <thor-internal/arch/system.hpp>

#include <thor-internal/pci/pcie_ecam.hpp>
#include <thor-internal/pci/pcie_brcmstb.hpp>

#ifndef __riscv
#include <thor-internal/arch/gic.hpp>
#endif

namespace thor {

extern frg::manual_box<IrqSlot> globalIrqSlots[numIrqSlots];

} // namespace thor

namespace thor::pci {

static constexpr bool logRoutingTable = false;

struct DtbPciIrqRouter : PciIrqRouter {
	DtbPciIrqRouter(PciIrqRouter *parent_, PciBus *associatedBus_, DeviceTreeNode *node);

	PciIrqRouter *makeDownstreamRouter(PciBus *bus) override;
};

DtbPciIrqRouter::DtbPciIrqRouter(PciIrqRouter *parent_, PciBus *associatedBus_,
		DeviceTreeNode *node)
: PciIrqRouter{parent_, associatedBus_} {
	if (!node) {
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

	auto &map = node->interruptMap();
	assert(map.size());

	auto mask = node->interruptMapMask()[0];

	auto ignored = (~mask & 0x0000F800);
	size_t nIgnoredComb = 1 << __builtin_popcount(ignored);

	// This bit of code maps values in [0, nIgnoredComb)
	// onto bits that are set in ignored.
	for (size_t i = 0; i < nIgnoredComb; i++) {
		auto disp = 0;
		auto n = 0;
		for (size_t j = 0; j < 16; j++) {
			if (ignored & (1 << j)) {
				disp |= ((i >> n) & 1) << j;
				n++;
			}
		}

#ifdef __aarch64
		// TODO: Get rid of this case, the one below is more general.
		for (auto ent : map) {
			auto addr = ent.childAddrHi + disp;
			uint32_t bus = (addr >> 16) & 0xFF;
			uint32_t slot = (addr >> 11) & 0x1F;
			uint32_t func = (addr >> 8) & 0x07;

			auto index = static_cast<IrqIndex>(ent.childIrq);

			assert(bus == associatedBus->busId);
			assert(!func && "TODO: support routing of individual functions");
			infoLogger() << "    Route for slot " << slot
					<< ", " << nameOf(index) << ": "
					<< "IRQ " << ent.parentIrq.id << " on "
					<< ent.interruptController->path() << frg::endlog;

			// TODO: care about polarity
			auto irq = ent.parentIrq.id;
			if (globalIrqSlots[irq]->isAvailable()) {
				// TODO: Do not assume the GIC here.
				auto pin = gic->setupIrq(irq, ent.parentIrq.trigger);
				routingTable.push({slot, index, pin});
			} else {
				auto pin = globalIrqSlots[irq]->pin();
				routingTable.push({slot, index, pin});
			}
		}
#else
		auto success = dt::walkInterruptMap([&] (
				dtb::Cells childAddress,
				dtb::Cells childIrq,
				DeviceTreeNode *parentNode,
				dtb::Cells parentAddress,
				dtb::Cells parentIrq) {
			if (childAddress.numCells() != 3)
				panicLogger() << "Expected three child address cells in ECAM interrupt-map" << frg::endlog;
			uint32_t bdf;
			if (!childAddress.readSlice(bdf, 0, 1))
				panicLogger() << "Failed to read BDF from ECAM interupt-map" << frg::endlog;
			auto addr = bdf + disp;
			uint32_t bus = (addr >> 16) & 0xFF;
			uint32_t slot = (addr >> 11) & 0x1F;
			uint32_t func = (addr >> 8) & 0x07;
			assert(bus == associatedBus->busId);
			assert(!func && "TODO: support routing of individual functions");

			uint32_t index;
			if (!childIrq.read(index))
				panicLogger() << "Failed to read pin index from interrupt-map" << frg::endlog;

			if (parentAddress.numCells())
				panicLogger() << "thor: ECAM interrupt-maps with non-zero parent #address-cells are unsupported" << frg::endlog;

			auto *irqController = parentNode->getAssociatedIrqController();
			if (!irqController)
				panicLogger() << "No IRQ controller associated with "
						<< parentNode->path() << frg::endlog;
			auto pin = irqController->resolveDtIrq(parentIrq);
			if (logRoutingTable)
				infoLogger() << bus << " " << slot << " [" << index << "]: Routed to IRQ "
						<< pin->name() << frg::endlog;
			routingTable.push({slot, static_cast<IrqIndex>(index), pin});
		}, node);
		if (!success)
			panicLogger() << "Failed to walk interrupt-map of " << node->path() << frg::endlog;
#endif
	}

	routingModel = RoutingModel::rootTable;
}

PciIrqRouter *DtbPciIrqRouter::makeDownstreamRouter(PciBus *bus) {
	return frg::construct<DtbPciIrqRouter>(*kernelAlloc, this, bus, nullptr);
}

namespace {

void initPciNode(DeviceTreeNode *node) {
	infoLogger() << "thor: Initializing node \"" << node->path() << "\":" << frg::endlog;

	PciConfigIo *io = nullptr;

	auto range = node->busRange();

	if (node->isCompatible<1>({"pci-host-ecam-generic"})) {
		infoLogger() << "thor:\tIt's a generic controller with ECAM IO." << frg::endlog;
		assert(node->reg().size() == 1);

		io = frg::construct<EcamPcieConfigIo>(*kernelAlloc,
			node->reg()[0].addr, 0,
			range.from, range.to);

	} else if (node->isCompatible<1>({"brcm,bcm2711-pcie"})) {
		infoLogger() << "thor:\tIt's a Broadcom STB PCIe controller." << frg::endlog;

		io = frg::construct<BrcmStbPcie>(*kernelAlloc,
			node, 0,
			range.from, range.to);
	}

	if (!io) {
		infoLogger() << "thor: Unsupported PCI(e) controller \"" << node->path() << "\"" << frg::endlog;
		return;
	}

	auto rootBus = frg::construct<PciBus>(*kernelAlloc, nullptr, nullptr, io, nullptr, 0, range.from);
	rootBus->irqRouter = frg::construct<DtbPciIrqRouter>(*kernelAlloc, nullptr, rootBus, node);

	for (auto &r : node->ranges()) {
		assert(r.childAddrHiValid);

		uint8_t type = (r.childAddrHi >> 24) & 0b11;

		uint32_t resFlags = 0;

		switch (type) {
			case 1:
				resFlags = PciBusResource::io;
				break;
			case 3:
			case 2:
				resFlags = PciBusResource::memory;

				if (r.childAddrHi & (1 << 30))
					resFlags = PciBusResource::prefMemory;

				break;
			default:
				infoLogger()
					<< "Unexpected range type "
					<< type << frg::endlog;
		}

		infoLogger() << "thor: Adding resource " << (void *)r.childAddr << " with flags " << resFlags << frg::endlog;

		rootBus->resources.push_back({r.childAddr, r.size,
				r.parentAddr, resFlags, true});
	}

	addRootBus(rootBus);
}

initgraph::Task discoverDtbNodes{&globalInitEngine, "pci.discover-dtb-nodes",
	initgraph::Requires{getDeviceTreeParsedStage()},
	initgraph::Entails{getRootsDiscoveredStage()},
	[] {
		size_t i = 0;

		getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
			if (node->isCompatible(dtPciCompatible)) {
				initPciNode(node);
				i++;
			}

			return false;
		});

		infoLogger() << "thor: Found " << i << " PCI nodes in total." << frg::endlog;
	}
};

} // namespace anonymous

} // namespace thor::pci
