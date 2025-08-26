#pragma once

#include <dtb.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <initgraph.hpp>
#include <thor-internal/dtb/irq.hpp>
#include <thor-internal/irq.hpp>
#include <frg/optional.hpp>
#include <frg/hash_map.hpp>
#include <frg/vector.hpp>
#include <frg/array.hpp>
#include <frg/span.hpp>
#include <cstddef>

namespace thor {

namespace dt {
struct IrqController;
struct MbusNode;

void publishNodes();

} // namespace dt

struct DeviceTreeNode {
	DeviceTreeNode(::DeviceTreeNode dtNode, DeviceTreeNode *parent)
	: dtNode_{dtNode}, parent_{parent}, children_{{}, *kernelAlloc}, name_{}, path_{*kernelAlloc},
	model_{}, phandle_{}, compatible_{*kernelAlloc}, addressCells_{2}, hasAddressCells_{false},
	sizeCells_{1}, hasSizeCells_{false}, interruptCells_{0}, hasInterruptCells_{false},
	reg_{*kernelAlloc}, ranges_{*kernelAlloc}, interruptController_{false},
	interruptParentId_{0}, interruptParent_{}, busRange_{0, 0xFF} { }

	void initializeWith(::DeviceTreeNode dtNode);
	void finalizeInit();

	void attachChild(frg::string_view name, DeviceTreeNode *node) {
		children_.insert(name, node);
	}

	const ::DeviceTreeNode &dtNode() const {
		return dtNode_;
	}

	DeviceTreeNode *parent() const {
		return parent_;
	}

	DeviceTreeNode *interruptParent() const {
		return interruptParent_;
	}

	frg::string_view name() const {
		return name_;
	}

	frg::string_view model() const {
		return model_;
	}

	const frg::vector<frg::string_view, KernelAlloc>& compatible() const {
		return compatible_;
	}

	template <size_t N>
	bool isCompatible(frg::array<frg::string_view, N> with) const {
		for (const auto &c : compatible_) {
			for (const auto &w : with) {
				if (c == w) {
					return true;
				}
			}
		}

		return false;
	}

	bool isInterruptController() const {
		return interruptController_;
	}

	struct RegRange {
		uint32_t addrHi;
		uint64_t addr;
		size_t size;

		bool addrHiValid;
	};

	struct BusRange {
		uint32_t from;
		uint32_t to;
	};

	struct AddrTranslateRange {
		uint32_t childAddrHi;
		uint64_t childAddr;
		uint64_t parentAddr;
		size_t size;

		bool childAddrHiValid;
	};

	frg::string_view path() const {
		return path_;
	}

	uint32_t phandle() {
		return phandle_;
	}

	uint64_t translateAddress(uint64_t addr) const;

	auto addressCells() const {
		return addressCells_;
	}
	bool hasAddressCells() const {
		return hasAddressCells_;
	}
	auto sizeCells() const {
		return sizeCells_;
	}
	auto interruptCells() const {
		return interruptCells_;
	}

	const auto &reg() const {
		return reg_;
	}

	const auto &ranges() const {
		return ranges_;
	}

	const auto &children() const {
		return children_;
	}

	const auto &busRange() const {
		return busRange_;
	}

	template <typename F>
	bool forEach(F &&func) {
		for (auto [_, child] : children_) {
			if (func(child))
				return true;
			if (child->forEach(std::forward<F>(func)))
				return true;
		}

		return false;
	}

	void associateIrqController(dt::IrqController *irqController) {
		associatedIrqController_ = irqController;
	}

	dt::IrqController *getAssociatedIrqController() {
		return associatedIrqController_;
	}

	void associateMbusNode(dt::MbusNode *node) {
		associatedMbusNode_ = node;
	}

	dt::MbusNode *getAssociatedMbusNode() {
		return associatedMbusNode_;
	}

private:
	void generatePath_();

	::DeviceTreeNode dtNode_;

	DeviceTreeNode *parent_;

	frg::hash_map<
		frg::string_view,
		DeviceTreeNode *,
		frg::hash<frg::string_view>,
		KernelAlloc
	> children_;

	frg::string_view name_;
	frg::string<KernelAlloc> path_;
	frg::string_view model_;
	uint32_t phandle_;
	frg::vector<frg::string_view, KernelAlloc> compatible_;

	int addressCells_;
	bool hasAddressCells_;
	int sizeCells_;
	bool hasSizeCells_;
	int interruptCells_;
	bool hasInterruptCells_;

	frg::vector<RegRange, KernelAlloc> reg_;
	frg::vector<AddrTranslateRange, KernelAlloc> ranges_;

	bool interruptController_;

	uint32_t interruptParentId_;
	DeviceTreeNode *interruptParent_;

	BusRange busRange_;

	// Kernel objects associated with this DeviceTreeNode.
	dt::IrqController *associatedIrqController_{nullptr};
	dt::MbusNode *associatedMbusNode_{nullptr};
};

DeviceTreeNode *getDeviceTreeNodeByPath(frg::string_view path);
DeviceTreeNode *getDeviceTreeNodeByPhandle(uint32_t phandle);
DeviceTreeNode *getDeviceTreeRoot();

initgraph::Stage *getDeviceTreeParsedStage();

static inline frg::array<frg::string_view, 12> dtGicV2Compatible = {
	"arm,arm11mp-gic",
	"arm,cortex-a15-gic",
	"arm,cortex-a7-gic",
	"arm,cortex-a5-gic",
	"arm,cortex-a9-gic",
	"arm,eb11mp-gic",
	"arm,gic-400",
	"arm,pl390",
	"arm,tc11mp-gic",
	"nvidia,tegra210-agic",
	"qcom,msm-8660-qgic",
	"qcom,msm-qgic2"
};

static inline frg::array<frg::string_view, 1> dtGicV3Compatible = {
	"arm,gic-v3"
};

static inline frg::array<frg::string_view, 3> dtPciCompatible = {
	"pci-host-cam-generic",
	"pci-host-ecam-generic",
	"brcm,bcm2711-pcie"
};

namespace dt {

template<typename Fn>
[[nodiscard]] frg::optional<bool> walkInterrupts(Fn fn, DeviceTreeNode *node) {
	auto prop = node->dtNode().findProperty("interrupts");
	if (!prop) {
		return frg::null_opt;
	}

	auto it = prop->access();
	while (it != dtb::endOfProperty) {
		auto *parentNode = node->interruptParent();
		auto parentInterruptCells = parentNode->interruptCells();

		dtb::Cells parentIrq;
		if (!it.intoCells(parentIrq, parentInterruptCells)) {
			warningLogger() << node->path() << ": failed to read parent IRQ from interrupts"
					<< frg::endlog;
			return false;
		}
		it += parentInterruptCells * sizeof(uint32_t);

		fn(parentNode, parentIrq);
	}

	return true;
}

template<typename Fn>
[[nodiscard]] bool walkInterruptsExtended(Fn fn, DeviceTreeNode *node) {
	auto prop = node->dtNode().findProperty("interrupts-extended");
	if (!prop) {
		warningLogger() << node->path() << " has no interrupts-extended" << frg::endlog;
		return false;
	}

	auto it = prop->access();
	while (it != dtb::endOfProperty) {
		uint32_t parentPhandle;
		if (!it.readCells(parentPhandle, 1)) {
			warningLogger() << node->path() << ": failed to read phandle from interrupts-extended"
					<< frg::endlog;
			return false;
		}
		it += sizeof(uint32_t);
		auto *parentNode = getDeviceTreeNodeByPhandle(parentPhandle);
		if (!parentNode) {
			warningLogger() << node->path() << ": no DT node with phandle " << parentPhandle
					<< frg::endlog;
			return false;
		}
		auto parentInterruptCells = parentNode->interruptCells();

		dtb::Cells parentIrq;
		if (!it.intoCells(parentIrq, parentInterruptCells)) {
			warningLogger() << node->path() << ": failed to read parent IRQ from interrupts-extended"
					<< frg::endlog;
			return false;
		}
		it += parentInterruptCells * sizeof(uint32_t);

		fn(parentNode, parentIrq);
	}

	return true;
}

template<typename Fn>
[[nodiscard]] bool walkInterruptMap(Fn fn, DeviceTreeNode *node) {
	auto prop = node->dtNode().findProperty("interrupt-map");
	if (!prop) {
		warningLogger() << node->path() << " has no interrupt-map" << frg::endlog;
		return false;
	}

	auto childAddressCells = node->addressCells();
	auto childInterruptCells = node->interruptCells();

	auto it = prop->access();
	while (it != dtb::endOfProperty) {
		dtb::Cells childAddress;
		dtb::Cells childIrq;
		if (!it.intoCells(childAddress, childAddressCells)) {
			warningLogger() << node->path() << ": failed to read child address from interrupt-map"
					<< frg::endlog;
			return false;
		}
		it += childAddressCells * sizeof(uint32_t);
		if (!it.intoCells(childIrq, childInterruptCells)) {
			warningLogger() << node->path() << ": failed to read child IRQ from interrupt-map"
					<< frg::endlog;
			return false;
		}
		it += childInterruptCells * sizeof(uint32_t);

		uint32_t parentPhandle;
		if (!it.readCells(parentPhandle, 1)) {
			warningLogger() << node->path() << ": failed to read phandle from interrupt-map"
					<< frg::endlog;
			return false;
		}
		it += sizeof(uint32_t);
		auto *parentNode = getDeviceTreeNodeByPhandle(parentPhandle);
		if (!parentNode) {
			warningLogger() << node->path() << ": no DT node with phandle " << parentPhandle
					<< frg::endlog;
			return false;
		}
		// NOTE: This behavior is not documented in the DT specification (the spec says the node
		// should explicitly set #address-cells to 0 if it needs to). This behavior is copied from
		// Linux, and is at least needed to correctly parse interrupt-map of the PCIe node on the RPi4.
		auto parentAddressCells = parentNode->hasAddressCells() ? parentNode->addressCells() : 0;
		auto parentInterruptCells = parentNode->interruptCells();

		dtb::Cells parentAddress;
		dtb::Cells parentIrq;
		if (!it.intoCells(parentAddress, parentAddressCells)) {
			warningLogger() << node->path() << ": failed to read parent address from interrupt-map"
					<< frg::endlog;
			return false;
		}
		it += parentAddressCells * sizeof(uint32_t);
		if (!it.intoCells(parentIrq, parentInterruptCells)) {
			warningLogger() << node->path() << ": failed to read parent IRQ from interrupt-map"
					<< frg::endlog;
			return false;
		}
		it += parentInterruptCells * sizeof(uint32_t);

		fn(childAddress, childIrq, parentNode, parentAddress, parentIrq);
	}

	return true;
}

} // namespace dt

} // namespace thor
