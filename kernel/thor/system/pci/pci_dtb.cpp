#include <thor-internal/io.hpp>
#include <thor-internal/kernel-heap.hpp>
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

	auto maskProp = node->dtNode().findProperty("interrupt-map-mask");
	if (!maskProp)
		panicLogger() << node->path() << " has no interrupt-map-mask" << frg::endlog;

	auto it = maskProp->access();
	uint32_t mask;
	if (!it.readCells(mask, 1))
		panicLogger() << node->path() << ": failed to read interrupt-map-mask field" << frg::endlog;

	// TODO(qookie): We mask off the function bits here.
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
			// Parent address does not matter in this case
			// (and is not present on QEMU's virt machine on AArch64).
			(void)parentAddress;

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

		auto root = getDeviceTreeRoot();
		if (!root)
			return;

		root->forEach([&](DeviceTreeNode *node) -> bool {
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
