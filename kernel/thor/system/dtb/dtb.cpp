#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <frg/manual_box.hpp>
#include <eir/interface.hpp>
#include <frg/hash.hpp>
#include <dtb.hpp>

namespace thor {

static inline constexpr bool logNodeInfo = false;

initgraph::Stage *getDeviceTreeParsedStage() {
	static initgraph::Stage s{&globalInitEngine, "dtb.tree-parsed"};
	return &s;
}

extern "C" EirInfo *thorBootInfoPtr;

namespace {
	frg::manual_box<DeviceTree> dt;

	frg::manual_box<
		frg::hash_map<
			uint32_t,
			DeviceTreeNode *,
			frg::hash<uint32_t>,
			KernelAlloc
		>
	> phandles;

	DeviceTreeNode *treeRoot;

	auto parseStringList(const ::DeviceTreeProperty &prop) {
		frg::vector<frg::string_view, KernelAlloc> list{*kernelAlloc};

		size_t i = 0;
		while (i < prop.size()) {
			frg::string_view sv{reinterpret_cast<const char *>(prop.data()) + i};
			i += sv.size() + 1;
			list.push_back(sv);
		}

		return list;
	}
} // namespace anonymous

void DeviceTreeNode::initializeWith(::DeviceTreeNode dtNode) {
	name_ = dtNode.name();
	generatePath_();

	if (auto p = dtNode.findProperty("phandle"); p) {
		phandle_ = p->asU32();
	} else if (auto p = dtNode.findProperty("linux,phandle"); p) {
		infoLogger() << "thor: warning: node \"" << name() << "\" uses legacy \"linux,phandle\" property!" << frg::endlog;
		phandle_ = p->asU32();
	}

	if (phandle_)
		phandles->insert(phandle_, this);

	for (auto prop : dtNode.properties()) {
		frg::string_view pn{prop.name()};

		if (pn == "model") {
			model_ = reinterpret_cast<const char *>(prop.data());
		} else if (pn == "compatible") {
			compatible_ = parseStringList(prop);
		} else if (pn == "#address-cells") {
			addressCells_ = prop.asU32();
			hasAddressCells_ = true;
		} else if (pn == "#size-cells") {
			sizeCells_ = prop.asU32();
			hasSizeCells_ = true;
		} else if (pn == "#interrupt-cells") {
			interruptCells_ = prop.asU32();
			hasInterruptCells_ = true;
		} else if (pn == "interrupt-parent") {
			interruptParentId_ = prop.asU32();
		} else if (pn == "interrupt-controller") {
			interruptController_ = true;
		} else if (pn == "reg") {
			auto addrCells = parent_->addressCells_;
			auto sizeCells = parent_->sizeCells_;

			size_t j = 0;
			while (j < prop.size()) {
				RegRange reg{};

				if (addrCells) {
					if (addrCells == 3) {
						reg.addrHi = prop.asPropArrayEntry(1, j);
						reg.addrHiValid = true;
						reg.addr = prop.asPropArrayEntry(addrCells - 1, j + 1);
						j += addrCells * 4;
					} else {
						if(j + addrCells * 4 > prop.size()) {
							infoLogger() << "thor: warning: node \"" << name() << "\": reg field isn't conforming to #addr-cells" << frg::endlog;
							reg.addr = prop.asPropArrayEntry((j + addrCells * 4 - prop.size()) / 4);
							reg_.push_back(reg);
							break;
						}
						reg.addr = prop.asPropArrayEntry(addrCells, j);
						j += addrCells * 4;
					}
				}

				if (sizeCells) {
					if(j + sizeCells * 4 > prop.size()) {
						infoLogger() << "thor: warning: node \"" << name() << "\": reg field isn't conforming to #size-cells" << frg::endlog;
						reg.size = prop.asPropArrayEntry((j + sizeCells * 4 - prop.size()) / 4);
						reg_.push_back(reg);
						break;
					}
					reg.size = prop.asPropArrayEntry(sizeCells, j);
					j += sizeCells * 4;
				}

				reg_.push_back(reg);
			}
		} else if (pn == "interrupts") {
			// This is parsed by the interrupt controller node
			irqData_ = {static_cast<const std::byte *>(prop.data()), prop.size()};
		} else if (pn == "interrupt-map") {
			interruptMapRaw_ = {static_cast<const std::byte *>(prop.data()), prop.size()};
		} else if (pn == "enable-method") {
			auto methods = parseStringList(prop);

			// Look for the first known method
			for (auto method : methods) {
				if (method == "spin-table") {
					enableMethod_ = EnableMethod::spintable;
					break;
				} else if (method == "psci") {
					enableMethod_ = EnableMethod::psci;
					break;
				}
			}
		} else if (pn == "cpu-release-addr") {
			cpuReleaseAddr_ = prop.asU64();
		} else if (pn == "method") {
			method_ = reinterpret_cast<const char *>(prop.data());
		} else if (pn == "cpu_on") {
			cpuOn_ = prop.asU32();
		} else if (pn == "bus-range") {
			busRange_.from = prop.asPropArrayEntry(1, 0);
			busRange_.to = prop.asPropArrayEntry(1, 4);
		}
	}

	// Iterate again to parse things that depend on previously parsed properties
	for (auto prop : dtNode.properties()) {
		frg::string_view pn{prop.name()};

		if (pn == "ranges") {
			auto parentAddrCells = parent_->addressCells_;
			auto childAddrCells = addressCells_;
			auto sizeCells = sizeCells_;

			size_t j = 0;
			while (j < prop.size()) {
				AddrTranslateRange reg{};
				// PCI(e) buses have a 3 cell long child addresses
				if (childAddrCells == 3) {
					reg.childAddrHi = prop.asPropArrayEntry(1, j);
					j += 4;
					reg.childAddr = prop.asPropArrayEntry(2, j);
					j += 8;
					reg.childAddrHiValid = true;
				} else {
					assert(childAddrCells < 3);
					reg.childAddr = prop.asPropArrayEntry(childAddrCells, j);
					j += childAddrCells * 4;
				}

				assert(parentAddrCells < 3);
				reg.parentAddr = prop.asPropArrayEntry(parentAddrCells, j);
				j += parentAddrCells * 4;

				reg.size = prop.asPropArrayEntry(sizeCells, j);
				j += sizeCells * 4;

				ranges_.push_back(reg);
			}
		} else if (pn == "interrupt-map-mask") {
			size_t size = interruptCells_ + addressCells_;
			for (size_t i = 0; i < size; i++) {
				interruptMapMask_.push_back(prop.asU32(i * 4));
			}
		}
	}

	{
		// inherit interrupt parent from parent if we don't have one
		auto parent = parent_;
		while (!interruptParentId_) {
			if (!parent)
				break;
			if (parent->isInterruptController()) {
				assert(parent->phandle_);
				interruptParentId_ = parent->phandle_;
				continue;
			}

			interruptParentId_ = parent->interruptParentId_;
			parent = parent->parent_;
		}
	}
}

void DeviceTreeNode::finalizeInit() {
	if (interruptParentId_ != 0) {
		auto ipIt = phandles->find(interruptParentId_);
		if (ipIt == phandles->end()) {
			panicLogger() << "thor: node \"" << name() << "\" has an interrupt parent id "
				<< interruptParentId_ << " but no such node exists" << frg::endlog;
		} else {
			interruptParent_ = ipIt->get<1>();
		}

		if (irqData_.size()) {
			irqs_ = interruptParent_->parseIrqs_(irqData_);
		}
	}

	// perform address translation
	if (parent_ && parent_->ranges_.size()) {
		for (auto &r : reg_) {
			r.addr = parent_->translateAddress(r.addr);
		}

		for (auto &r : ranges_) {
			r.parentAddr = parent_->translateAddress(r.parentAddr);
		}
	}

	// parse interrupt-map
	if (interruptMapRaw_.data()) {
		auto childAddrCells = addressCells_;
		auto nexusInterruptCells = interruptCells_;

		::DeviceTreeProperty prop{"", interruptMapRaw_};

		size_t j = 0;
		while (j < prop.size()) {
			InterruptMapEntry entry;
			// PCI(e) buses have a 3 cell long child addresses
			if (childAddrCells == 3) {
				entry.childAddrHi = prop.asPropArrayEntry(1, j);
				j += 4;
				entry.childAddr = prop.asPropArrayEntry(2, j);
				j += 8;
				entry.childAddrHiValid = true;
			} else {
				assert(childAddrCells < 3);
				entry.childAddr = prop.asPropArrayEntry(childAddrCells, j);
				j += childAddrCells * 4;
			}

			entry.childIrq = prop.asPropArrayEntry(nexusInterruptCells, j);
			j += nexusInterruptCells * 4;

			auto phandle = prop.asPropArrayEntry(1, j);
			j += 4;

			auto intParent = (*phandles)[phandle];
			entry.interruptController = intParent;

			auto parentAddrCells = intParent->hasAddressCells_
				? intParent->addressCells_
				: 0;
			auto parentInterruptCells = intParent->interruptCells_;

			assert(parentAddrCells < 3);
			entry.parentAddr = prop.asPropArrayEntry(parentAddrCells, j);
			j += parentAddrCells * 4;

			entry.parentIrq = intParent->parseIrq_(&prop, j);
			j += parentInterruptCells * 4;

			interruptMap_.push_back(entry);
		}
	}

	if (logNodeInfo &&
			(irqs_.size()
			 || reg_.size()
			 || ranges_.size())) {
		infoLogger() << "Node \"" << path() << "\" has the following:" << frg::endlog;

		if (compatible_.size()) {
			infoLogger() << "\t- compatible names:" << frg::endlog;
			for (auto c : compatible_) {
				infoLogger() << "\t\t- " << c << frg::endlog;
			}
		}

		if (irqs_.size()) {
			constexpr const char *polarityNames[] = {"null", "high", "low"};
			constexpr const char *triggerNames[] = {"null", "edge", "level"};

			infoLogger() << "\t- interrupts:" << frg::endlog;
			for (auto irq : irqs_) {
				infoLogger() << "\t\t- ID: " << irq.id << ", polarity: "
					<< polarityNames[(int)irq.polarity]
					<< ", trigger: " << triggerNames[(int)irq.trigger]
					<< frg::endlog;
			}
		}

		if (reg_.size()) {
			infoLogger() << "\t- resources:" << frg::endlog;
			for (auto reg : reg_) {
				if (reg.size)
					infoLogger() << "\t\t- " << (void *)reg.addr << " - "
						<< (void *)reg.size << " bytes" << frg::endlog;
				else
					infoLogger() << "\t\t- " << (void *)reg.addr << frg::endlog;
			}
		}

		if (ranges_.size()) {
			infoLogger() << "\t- ranges:" << frg::endlog;
			for (auto range : ranges_) {
				if (range.childAddrHiValid && isCompatible(dtPciCompatible)) {
					bool pref = range.childAddrHi & (1 << 30);
					uint8_t type = (range.childAddrHi >> 24) & 0b11;

					constexpr const char *typeNames[] = {"config", "I/O", "32-bit memory", "64-bit memory"};

					infoLogger() << "\t\t- child (" << (pref ? "" : "non-")
						<< "prefetchable, " << typeNames[type] << ") "
						<< (void *)range.childAddr << " translates to host "
						<< (void *)range.parentAddr << " - "
						<< (void *)range.size << " bytes" << frg::endlog;
				} else {
					infoLogger() << "\t\t- child " << (void *)range.childAddr << " translates to host "
						<< (void *)range.parentAddr << " - "
						<< (void *)range.size << " bytes" << frg::endlog;
				}
			}
		}

		if (interruptMap_.size()) {
			constexpr const char *pciPins[] = {"null", "#INTA", "#INTB", "#INTC", "#INTD"};

			infoLogger() << "\t- interrupt mappings:" << frg::endlog;
			for (auto ent : interruptMap_) {
				if (ent.childAddrHiValid && isCompatible(dtPciCompatible)) {
					infoLogger() << "\t\t- " << pciPins[ent.childIrq] << " of "
						<< frg::hex_fmt{ent.childAddrHi} << " to " << ent.parentIrq.id << " of "
						<< ent.interruptController->path() << frg::endlog;
				}
			}
		}
	}

	// Recurse into children
	for (auto &[_, child] : children_)
		child->finalizeInit();
}

auto DeviceTreeNode::parseIrq_(::DeviceTreeProperty *prop, size_t i) -> DeviceIrq {
	DeviceIrq irq{};
	// TODO: This code assumes the GIC.
	//       Revise it to simply store a reference to the property in DeviceIrq.
#ifndef __riscv
	bool isPPI = prop->asU32(i);
	uint32_t rawId = prop->asU32(i + 4);
	uint32_t flags = prop->asU32(i + 8);

	if (isPPI)
		irq.id = rawId + 16;
	else
		irq.id = rawId + 32;

	switch (flags & 0xF) {
		case 1:
			irq.polarity = Polarity::high;
			irq.trigger = TriggerMode::edge;
			break;
		case 2:
			irq.polarity = Polarity::low;
			irq.trigger = TriggerMode::edge;
			break;
		case 4:
			irq.polarity = Polarity::high;
			irq.trigger = TriggerMode::level;
			break;
		case 8:
			irq.polarity = Polarity::low;
			irq.trigger = TriggerMode::level;
			break;
		default:
			infoLogger() << "thor: Illegal IRQ flags " << (flags & 0xF)
				<< " found when parsing IRQ property"
				<< frg::endlog;
			irq.polarity = Polarity::null;
			irq.trigger = TriggerMode::null;
	}

	irq.ppiCpuMask = isPPI ? ((flags >> 8) & 0xFF) : 0;
#endif
	return irq;
}

auto DeviceTreeNode::parseIrqs_(frg::span<const std::byte> data) -> frg::vector<DeviceIrq, KernelAlloc> {
	frg::vector<DeviceIrq, KernelAlloc> ret{*kernelAlloc};

	::DeviceTreeProperty prop{"", data};

	// We only support GIC irqs for now
	if (!isCompatible(dtGicV2Compatible) && !isCompatible(dtGicV3Compatible)) {
		infoLogger() << "thor: warning: Skipping parsing IRQs using node \"" << path()
			<< "\", it's not compatible with the GIC" << frg::endlog;
		return ret;
	}

	assert(interruptCells_ >= 3);

	size_t j = 0;
	while (j < prop.size()) {
		ret.push_back(parseIrq_(&prop, j));
		j += interruptCells_ * 4;
	}

	return ret;
}

void DeviceTreeNode::generatePath_() {
	frg::vector<frg::string_view, KernelAlloc> components{*kernelAlloc};

	auto p = this;
	while (p) {
		components.push_back(p->name());
		p = p->parent();
	}

	for (int i = components.size() - 1; i >= 0; i--) {
		if (components[i].size())
			path_ += "/";
		path_ += components[i];
	}
}

uint64_t DeviceTreeNode::translateAddress(uint64_t addr) const {
	// We only handle simple bus address translation
	if (!isCompatible<1>({"simple-bus"}))
		return addr;

	// This node has no translation table
	if (!ranges_.size())
		return addr;

	for (auto tr : ranges_)
		if (addr >= tr.childAddr && addr <= (tr.childAddr + tr.size))
			return tr.parentAddr + (addr - tr.childAddr);

	panicLogger() << "thor: address " << (void *)addr
		<< " doesn't fall into any of \""
		<< path() << "\"'s memory ranges" << frg::endlog;
	__builtin_unreachable();
}

DeviceTreeNode *getDeviceTreeNodeByPath(frg::string_view path) {
	auto remaining = path.sub_string(1, path.size() - 1);
	auto p = treeRoot;

	// remove null terminator
	if (!remaining[remaining.size() - 1])
		remaining = remaining.sub_string(0, remaining.size() - 1);

	while (remaining.size()) {
		auto nextSlash = remaining.find_first('/');
		if (nextSlash == size_t(-1))
			nextSlash = remaining.size();
		auto component = remaining.sub_string(0, nextSlash);

		auto it = p->children().find(component);

		if (it == p->children().end())
			return nullptr;
		else
			p = it->get<1>();

		if (nextSlash == remaining.size())
			break;
		remaining = remaining.sub_string(nextSlash + 1, remaining.size() - nextSlash - 1);
	}

	return p;
}

DeviceTreeNode *getDeviceTreeRoot() {
	return treeRoot;
}

static initgraph::Task initTablesTask{&globalInitEngine, "dtb.parse-dtb",
	initgraph::Entails{getDeviceTreeParsedStage()},
	[] {
		size_t dtbPageOff = thorBootInfoPtr->dtbPtr & (kPageSize - 1);
		size_t dtbSize = (thorBootInfoPtr->dtbSize + dtbPageOff + kPageSize - 1) & ~(kPageSize - 1);

		auto ptr = KernelVirtualMemory::global().allocate(dtbSize);
		uintptr_t va = reinterpret_cast<uintptr_t>(ptr);
		uintptr_t pa = thorBootInfoPtr->dtbPtr & ~(kPageSize - 1);

		for (size_t i = 0; i < dtbSize; i += kPageSize) {
			KernelPageSpace::global().mapSingle4k(va, pa,
				page_access::write, CachingMode::null);
			va += kPageSize;
			pa += kPageSize;
		}

		ptr = reinterpret_cast<void *>(
				reinterpret_cast<uintptr_t>(ptr) + dtbPageOff);

		dt.initialize(ptr);
		phandles.initialize(frg::hash<uint32_t>{}, *kernelAlloc);

		treeRoot = frg::construct<DeviceTreeNode>(*kernelAlloc, nullptr);
		treeRoot->initializeWith(dt->rootNode());

		infoLogger() << "thor: Booting on \"" << treeRoot->model() << "\"" << frg::endlog;

		struct {
			DeviceTreeNode *curr;

			void push(::DeviceTreeNode node) {
				auto n = frg::construct<DeviceTreeNode>(*kernelAlloc, curr);

				n->initializeWith(node);
				curr->attachChild(node.name(), n);
				curr = n;
			}

			void pop() {
				curr = curr->parent();
			}
		} walker{treeRoot};

		dt->rootNode().walkChildren(walker);

		// Initialize interruptParent etc
		// This can't be done above because the interrupt parent may not
		// have been discovered yet
		treeRoot->finalizeInit();
	}
};

} // namespace thor
