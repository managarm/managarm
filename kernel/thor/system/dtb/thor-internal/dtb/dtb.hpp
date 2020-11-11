#pragma once

#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/initgraph.hpp>
#include <thor-internal/irq.hpp>
#include <frg/hash_map.hpp>
#include <frg/vector.hpp>
#include <frg/array.hpp>
#include <frg/span.hpp>

// forward decl of types from kernel/common/dtb.hpp
struct DeviceTreeNode;

namespace thor {

struct DeviceTreeNode {
	DeviceTreeNode(DeviceTreeNode *parent)
	: parent_{parent}, children_{{}, *kernelAlloc}, name_{}, path_{*kernelAlloc},
	model_{}, phandle_{}, compatible_{*kernelAlloc}, addressCells_{2}, sizeCells_{1},
	interruptCells_{}, reg_{*kernelAlloc}, ranges_{*kernelAlloc}, irqData_{nullptr, 0},
	irqs_{*kernelAlloc}, interruptController_{false}, interruptParentId_{0},
	interruptParent_{} { }

	void initializeWith(::DeviceTreeNode dtNode);
	void finalizeInit();

	void attachChild(frg::string_view name, DeviceTreeNode *node) {
		children_.insert(name, node);
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
		uint64_t addr;
		size_t size;
	};

	struct AddrTranslateRange {
		uint32_t childAddrHi;
		uint64_t childAddr;
		uint64_t parentAddr;
		size_t size;

		bool childAddrHiValid;
	};

	struct DeviceIrq {
		uint32_t id;
		Polarity polarity;
		TriggerMode trigger;

		// GIC-specific
		uint8_t ppiCpuMask;
	};

	frg::string_view path() const {
		return path_;
	}

	uint64_t translateAddress(uint64_t addr) const;

	const auto &reg() const {
		return reg_;
	}

	const auto &ranges() const {
		return ranges_;
	}

	const auto &children() const {
		return children_;
	}

	const auto &irqs() const {
		return irqs_;
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

private:
	frg::vector<DeviceIrq, KernelAlloc> parseIrqs_(frg::span<const void> prop);
	void generatePath_();

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
	int sizeCells_;
	int interruptCells_;

	frg::vector<RegRange, KernelAlloc> reg_;
	frg::vector<AddrTranslateRange, KernelAlloc> ranges_;

	frg::span<const void> irqData_;
	frg::vector<DeviceIrq, KernelAlloc> irqs_;

	bool interruptController_;

	uint32_t interruptParentId_;
	DeviceTreeNode *interruptParent_;
};

DeviceTreeNode *getDeviceTreeNodeByPath(frg::string_view path);
DeviceTreeNode *getDeviceTreeRoot();

initgraph::Stage *getDeviceTreeParsedStage();

static inline frg::array<frg::string_view, 12> dtGicCompatible = {
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

static inline frg::array<frg::string_view, 3> dtPciCompatible = {
	"pci-host-cam-generic",
	"pci-host-ecam-generic",
	"brcm,bcm2711-pcie"
};

} // namespace thor
